#include "check_alias_annotation.h"

namespace torch {
namespace jit {
namespace {

IValue deepCopy(const IValue& self) {
  // primitive types can be copied directly
  if (!self.isPtrType()) {
    return self;
  }

  // Tensors need special handling, since copy assignment creates an alias
  if (self.isTensor()) {
    return IValue(self.toTensor().clone());
  }
  if (self.isTensorList()) {
    std::vector<at::Tensor> newList;
    for (const auto& oldTensor : self.toTensorListRef()) {
      newList.push_back(oldTensor.clone());
    }
    return newList;
  }

  // Lists of ivalues should recursively deep copy their contents
  if (self.isGenericList()) {
    std::vector<IValue> newList;
    for (const auto& value : self.toGenericListRef()) {
      newList.push_back(deepCopy(value));
    }
    return newList;
  }

  // Regular lists can copy assign
  if (self.isIntList()) {
    return IValue(self.toIntListRef());
  } else if (self.isDoubleList()) {
    return IValue(self.toDoubleListRef());
  } else if (self.isBoolList()) {
    return IValue(self.toBoolListRef());
  } else if (self.isString()) {
    return IValue(self.toStringRef());
  }

  // If in the future we add more reference types that are used in aten ops,
  // we'll have to add them as cases here.
  AT_ASSERT(false);
}

Stack deepCopy(const Stack& stack) {
  Stack ret;
  for (const auto& v : stack) {
    ret.push_back(deepCopy(v));
  }
  return ret;
}

bool deepEquals(const IValue& lhs, const IValue& rhs) {
  // only check tensors for now
  if (!lhs.isTensor() || !rhs.isTensor()) {
    return true;
  }

  return lhs.toTensor().equal(rhs.toTensor());
}

struct AliasAndIValue {
  AliasAndIValue(
      const c10::optional<at::AliasInfo>& aliasInfo,
      const IValue& iValue)
      : aliasInfo(aliasInfo), iValue(iValue) {}

  const c10::optional<at::AliasInfo>& aliasInfo;
  const IValue& iValue;
};

// No inputs should alias each other
void checkInputPreconditions(const Stack& inputs) {
  for (size_t i = 0; i < inputs.size(); i++) {
    for (size_t j = 0; j < inputs.size(); j++) {
      if (i == j) {
        continue;
      }
      const auto& lhs = inputs.at(i);
      const auto& rhs = inputs.at(j);
      JIT_ASSERT(!lhs.isAliasOf(rhs));
    }
  }
}

// If two ivalues alias, they must share an alias set
void checkAliases(
    const std::vector<AliasAndIValue>& inputs,
    const std::vector<AliasAndIValue>& outputs) {
  for (const auto& output : outputs) {
    // if this output aliases any input, make sure that they share an alias set
    for (const auto& input : inputs) {
      if (output.iValue.isAliasOf(input.iValue)) {
        const auto inputSet = input.aliasInfo;
        const auto outputSet = output.aliasInfo;
        JIT_ASSERT(inputSet && outputSet);
        JIT_ASSERT(inputSet->isSubsetOf(*outputSet));
      }
    }
  }
}

// If we didn't specify that we write to an input value, it must have not
// changed
void checkWrites(
    const std::vector<AliasAndIValue>& inputs,
    const std::vector<IValue>& deepCopiedInputs) {
  JIT_ASSERT(inputs.size() == deepCopiedInputs.size());
  for (size_t i = 0; i < inputs.size(); i++) {
    const auto& input = inputs[i];
    const auto& deepCopiedInput = deepCopiedInputs[i];
    if (!input.aliasInfo || !input.aliasInfo->isWrite()) {
      JIT_ASSERT(deepEquals(input.iValue, deepCopiedInput));
    }
  }
}

const Node* findNodeForOp(
    const Graph& g,
    const std::string& unqualifiedOpName) {
  const auto opName = Symbol::fromQualString("aten::" + unqualifiedOpName);
  for (const auto node : g.nodes()) {
    if (node->kind() == opName) {
      return node;
    }
  }
  JIT_ASSERT(false);
}

// Handle a few special cases where we need to propagate constants
// manually
// TODO(suo): we should be able to move this stuff to constant prop
c10::optional<IValue> toIValueProp(const Value* v) {
  if (v->node()->kind() == prim::ListConstruct) {
    std::vector<IValue> genericList;
    for (auto input : v->node()->inputs()) {
      if (auto elem = toIValue(input)) {
        genericList.push_back(*elem);
      } else {
        // One of the list elements isn't constant.
        return c10::nullopt;
      }
    }

    // Specialize the list based on ListConstruct's return type
    auto listType = v->node()->output()->type();
    auto containedType = listType->containedTypes().at(0);
    if (containedType == IntType::get()) {
      return fmap(genericList, [](const IValue& v) { return v.toInt(); });
    } else if (containedType == FloatType::get()) {
      return fmap(genericList, [](const IValue& v) { return v.toDouble(); });
    } else if (containedType->isSubtypeOf(DynamicType::get())) {
      return fmap(genericList, [](const IValue& v) { return v.toTensor(); });
    } else {
      return c10::nullopt;
    }
  }

  if (v->node()->kind() == prim::StringToFloat) {
    auto op = getOperation(v->node());
    if (auto input = toIValue(v->node()->input())) {
      auto op = getOperation(v->node());
      Stack stack;
      push(stack, *input);
      op(stack);
      return stack.back();
    } else {
      return c10::nullopt;
    }
  }
  return c10::nullopt;
}
} // namespace

void checkAliasAnnotation(
    std::shared_ptr<Graph> graph,
    std::vector<IValue> pythonInputs,
    const std::string& unqualifiedOpName) {
  // Find the node that corresponds to our op name
  const auto node = findNodeForOp(*graph, unqualifiedOpName);

  // Build the stack to use as input to the op
  Stack stack;
  for (const auto input : node->inputs()) {
    if (input->node() == graph->param_node()) {
      // This value was passed as an input in python
      push(stack, pythonInputs.at(input->offset()));
    } else {
      // This a generated constant, which we need to evaluate
      auto inputValue = toIValue(input);
      if (!inputValue) {
        inputValue = toIValueProp(input);
      }

      if (inputValue) {
        push(stack, *inputValue);
      } else {
        JIT_ASSERT(input->type()->kind() == TypeKind::OptionalType);
        push(stack, IValue());
      }
    }
  }

  // Precondition: no inputs should alias each other. So if we find an alias,
  // it was created by the op.
  checkInputPreconditions(stack);

  const auto schema = node->schema();

  std::vector<AliasAndIValue> inputsToCheck;
  for (size_t i = 0; i < schema.arguments().size(); i++) {
    inputsToCheck.emplace_back(
        schema.arguments().at(i).alias_info(), stack.at(i));
  }

  // Save a copy of the inputs so we can check whether the original inputs were
  // written to.
  const auto inputsDeepCopy = deepCopy(stack);

  // Run the op
  getOperation(node)(stack);

  const auto outputs = std::move(stack);

  std::vector<AliasAndIValue> outputsToCheck;
  for (size_t i = 0; i < schema.returns().size(); i++) {
    outputsToCheck.emplace_back(
        schema.returns().at(i).alias_info(), outputs.at(i));
  }

  // Check that if any alias was created, we annotated it properly.
  checkAliases(inputsToCheck, outputsToCheck);

  // Check that if nothing was accidentally written to.
  checkWrites(inputsToCheck, inputsDeepCopy);
}

} // namespace jit
} // namespace torch
