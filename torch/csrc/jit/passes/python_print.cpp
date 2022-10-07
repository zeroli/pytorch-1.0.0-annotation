#include "torch/csrc/jit/passes/python_print.h"
#include "torch/csrc/jit/attributes.h"
#include "torch/csrc/jit/generic_if.h"
#include "torch/csrc/jit/ir.h"
#include "torch/csrc/jit/ir_views.h"
#include "torch/csrc/jit/export.h"
#include "torch/csrc/jit/resource_guard.h"
#include "torch/csrc/jit/script/error_report.h"
#include "torch/csrc/jit/script/module.h"


namespace torch {
namespace jit {

// unix isprint but insensitive to locale
static bool isPrint(char s) {
  return s > 0x1f && s < 0x7f;
}

void printQuotedString(std::ostream& stmt, const std::string& str) {
  stmt << "\"";
  for(auto s : str) {
    switch (s) {
      case '\\':
        stmt << "\\\\";
        break;
      case '\'':
        stmt << "\\'";
        break;
      case '\"':
        stmt << "\\\"";
        break;
      case '\a':
        stmt << "\\a";
        break;
      case '\b':
        stmt << "\\b";
        break;
      case '\f':
        stmt << "\\f";
        break;
      case '\n':
        stmt << "\\n";
        break;
      case '\r':
        stmt << "\\r";
        break;
      case '\t':
        stmt << "\\t";
        break;
      case '\v':
        stmt << "\\v";
        break;
      default:
        if (isPrint(s)) {
          stmt << s;
        } else {
          // C++ io has stateful formatting settings. Messing with
          // them is probably worse than doing this manually.
          char buf[4] = "000";
          buf[2] += s % 8; s /= 8;
          buf[1] += s % 8; s /= 8;
          buf[0] += s;
          stmt << "\\" << buf;
        }
        break;
    }
  }
  stmt << "\"";
}

static bool isValidIdentifierChar(char c, size_t pos) {
  return islower(c) || isupper(c) || c == '_' ||  (pos > 0 && isdigit(c));
}

static bool isValidIdentifier(const std::string & name) {
  if (name.size() == 0)
    return false;
  for (size_t i = 0; i < name.size(); ++i) {
    if (!isValidIdentifierChar(name[i], i))
      return false;
  }
  return true;
}

// handles names of the form, e.g., self.a.b
// if a field is not a valid identifier, then it will print as, e.g.
// getattr(self, "0").b
struct QualifiedName;
using QualifiedNamePtr = c10::intrusive_ptr<QualifiedName>;
struct QualifiedName : c10::intrusive_ptr_target {
  QualifiedName(QualifiedNamePtr prefix, std::string name)
  : prefix_(std::move(prefix)), name_(std::move(name)) {}
  QualifiedNamePtr prefix_;
  std::string name_;
  static QualifiedNamePtr create(QualifiedNamePtr prefix, std::string name) {
    return c10::make_intrusive<QualifiedName>(std::move(prefix), std::move(name));
  }
  static QualifiedNamePtr create(std::string name) {
    return c10::make_intrusive<QualifiedName>(QualifiedNamePtr(), std::move(name));
  }
  std::string str() const {
    std::stringstream ss;
    emit(ss);
    return ss.str();
  }
private:
  void emit(std::ostream& out) const {
    if (isValidIdentifier(name_)) {
      if (prefix_) {
        prefix_->emit(out);
        out << ".";
      }
      out << name_;
    } else {
      JIT_ASSERT(prefix_);
      out << "getattr(";
      prefix_->emit(out);
      out << ", ";
      printQuotedString(out, name_);
      out << ")";
    }
  }
};

void createTensorToParameterNameMap(
    const script::Module& module,
    QualifiedNamePtr prefix,
    std::unordered_map<at::Tensor*, QualifiedNamePtr>& result) {

  for (const auto& elem : module.get_parameters()) {
    const script::NamedParameter& param = elem.value();
    result[param.slot()] = QualifiedName::create(prefix, param.name);
  }
  for (const auto& elem : module.get_modules()) {
    createTensorToParameterNameMap(
        *elem->module, QualifiedName::create(prefix, elem.key()), result);
  }
}

  // some names are valid identifiers but off limits because
  // they are keywords or namespaces used in the output
  const static std::unordered_set<std::string> reserved_names = {
    // identifiers in the environment while parsing
    "aten",
    "ops",
    "CONSTANTS",
    "fork",
    "attribute",
    "getattr",
    "_", // avoid the confusing unnamed _
    "inf",
    "nan",
    // the python keywords
    "False",
    "None",
    "True",
    "and",
    "as",
    "assert",
    "break",
    "class",
    "continue",
    "def",
    "del",
    "elif",
    "else",
    "except",
    "finally",
    "for",
    "from",
    "global",
    "if",
    "import",
    "in",
    "is",
    "lambda",
    "nonlocal",
    "not",
    "or",
    "pass",
    "raise",
    "return",
    "try",
    "while",
    "with",
    "yield",
  };

struct PythonPrintPass {
  std::ostream& out;

  // constants are written to this table, and given then named CONSTANTS.cN
  // where N is the index into this table.

  std::vector<at::Tensor>& tensor_table_;
  // When printing this node, is it safe to write it inline (i.e. without
  // assigning a temporary variable
  std::unordered_set<Node*> output_inline_;

  // when we print this, should we error if the resulting output would
  // not be able to be reparsed?
  bool enforce_importable_;

  // what valid identifiers are in use for the current function
  std::unordered_set<std::string> used_names_;

  // used method names
  std::unordered_set<std::string> used_method_names_;

  // for fork,
  // subgraphs get added to the worklist, and will be printed later
  std::vector<std::function<void(void)>> worklist;

  // scanValue, scanNode, scanBlock:
  // decide if it is safe to omit the output of a temporary variable,
  // and inline the expression into its use
  // we only do this if
  // (1) it is a constant, or
  // (2) the temporary is unnamed, is single output, is used once,
  //     and would appear in the same order when the expression tree is reparsed.
  // The last case can be checked
  // becuase when we emit a expresion tree in the parser,
  // we do a left-to-right postorder traversal of the expression tree (emit children, then emit op).
  // The reverse of this is a right-to-left preorder traversal of the tree.
  // By doing a right-to-left preorder traversal of the inputs of a node,
  // while also scanning the list of emitted nodes backward, we can see if
  // they line up with what would happen when parsed the node as an expression. While they line
  // up we collapse them into an inline expression.

  // The inductive step is that the right-most input should be produced by the node
  // immediatly before the current node if it is in tree order.

  bool isConstantLike(Node* n) {
    switch(n->kind()) {
      case prim::Constant:
      case prim::NoneGenerator:
      case prim::Undefined:
      case prim::None:
        return true;
      default:
        return false;
    }
  }

  bool canInline(Value* v) {
    Node* n = v->node();
    // there must be only 1 values, otherwise we need an assignment to handle the multiple outout values
    if (n->outputs().size() != 1)
      return false;
    // if it is used more than once, then we need a variable
    if (v->uses().size() != 1)
      return false;
    auto use = v->uses().at(0);
    // if it has a name set, then it was written as a variable so preserve that
    // unless it is being fed directly to the end of the block.
    // in which case it is not as useful to give it a name just to return it
    if (v->hasUniqueName() && use.user->kind() != prim::Return)
      return false;
    // don't try to inline control blocks
    if (n->blocks().size() != 0)
      return false;
    // if it is a loop-carried input, we need a variable
    // otherwise the condition or trip count may be emitted in the wrong order w.r.t. to it
    if (use.user->kind() == prim::Loop && use.offset >= 2)
      return false;
    return true;
  }

  // block_point is the current node in the reverse linear scan of the emitted nodes
  // v is the current value in the tree traversal that may match with block_point's output.
  Node* scanValue(Node* block_point, Value* v) {
    Node* n = v->node();
    JIT_ASSERT(isConstantLike(n) || output_inline_.count(n) == 0);

    if (n == block_point && canInline(v)) { // the node must be at the expected point of the typical tree traversal
      // recursively see if we can inline the inputs to this input
      block_point = scanNode(block_point);
      output_inline_.insert(n);
    } else if (isConstantLike(n)) {
      // constant nodes can always be inlined, we will de-dup them on parsing
      // and put them at the top of the function regardless
      output_inline_.insert(n);
    }
    return block_point;
  }
  Node* previousNonConstant(Node* n) {
    do {
      n = n->prev();
    } while(isConstantLike(n));
    return n;
  }

  Node* scanNode(Node* n) {
    // don't bother to scan nodes we have already determined to be inline
    if(output_inline_.count(n)) {
      return n;
    }
    for(auto b : n->blocks()) {
      scanBlock(b);
    }
    Node* block_point = previousNonConstant(n);
    for(auto it = n->inputs().rbegin(),
             end = n->inputs().rend(); it != end; ++it) {
      block_point = scanValue(block_point, *it);
    }
    return block_point;
  }

  void scanBlock(Block* b) {
    scanNode(b->return_node());
    for(auto node : b->nodes().reverse()) {
      scanNode(node);
    }
  }

  size_t getOrAddTensorConstant(at::Tensor t) {
    // XXX - N^2 warning. This code does the exact same thing as
    // ConstantPool, which is also N^2 in the size of the constants,
    // because it doesn't hash any information about the tensors.
    // We will probably need to optimize this at some point using hashing.
    for(size_t i = 0; i < tensor_table_.size(); ++i) {
      if (t.type() == tensor_table_[i].type() && t.equal(tensor_table_[i])) {
        return i;
      }
    }
    JIT_ASSERT(t.is_variable());
    tensor_table_.emplace_back(std::move(t));
    return tensor_table_.size() - 1;
  }

  std::unordered_set<Node*> seen_constants;
  void buildConstantList(Node* n, std::vector<Node*>& constants) {
    for(auto input : n->inputs()) {
      if (isConstantLike(input->node()) && seen_constants.count(input->node()) == 0) {
        constants.push_back(input->node());
        seen_constants.insert(input->node());
      }
    }
    for(auto b : n->blocks()) {
      buildConstantList(b, constants);
    }
  }
  void buildConstantList(Block* b, std::vector<Node*>& constants) {
    for(auto n : b->nodes())
      buildConstantList(n, constants);
    buildConstantList(b->return_node(), constants);
  }
  // get a new name unique across calls to uniqueName() and
  // anything we have used.
  size_t next_id = 0;

  std::string genNameImpl(const std::string& candidate, std::unordered_set<std::string>& used) {
    std::string name = candidate;
    while(used.count(name) || reserved_names.count(name)) {
      name = candidate + std::to_string(next_id++);
    }
    used.insert(name);
    return name;
  }
  std::string genName(const std::string& candidate) {
    return genNameImpl(candidate, used_names_);
  }

  // methods self.foo are in a different namespace than
  // global identifiers, so they have a different procedure for finding a
  // uniquename
  std::string genMethodName(const std::string& candidate) {
    return genNameImpl(candidate, used_method_names_);
  }

  // unique names might not be valid identifiers,
  // force them to be by rewriting them
  static std::string makeValidIdentifier(const std::string& candidate) {
    std::stringstream ss;
    if (candidate.size() == 0 || isdigit(candidate[0]))
      ss << "_";
    for(char c : candidate) {
      if (isupper(c) || islower(c) || isdigit(c) || c == '_')
        ss << c;
      else
        ss << '_';
    }
    return ss.str();
  }
  // if we have to assign 'v' a name, what should it be?
  // use the uniqueName if it was set, otherwise generate a name.
  std::string genUniqueNameFor(Value* v) {
    return genName(
        v->hasUniqueName() ? makeValidIdentifier(v->uniqueName()) : "_");
  }

  // map from Value to how it should be printed at each use
  std::unordered_map<Value*, std::string> value_names_;

  std::string useOf(Value* v) const {
    return value_names_.at(v);
  }
  void assignValue(Value* v, const std::string& s) {
    value_names_[v] = s;
  }
  void assignValue(Value* v, Value* w) {
    assignValue(v, useOf(w));
  }
  void assignValuesToTheirUniqueNames(at::ArrayRef<Value*> values) {
    for(auto v : values) {
      assignValue(v, genUniqueNameFor(v));
    }
  }

  size_t level = 0;
  // indent to the current indent level
  std::ostream& indent() {
    for (size_t i = 0; i < level; ++i) {
      out << "  ";
    }
    return out;
  }

  ResourceGuard WithIndented() {
    level++;
    return ResourceGuard([this]{
      level--;
    });
  }

  template <class T0, class T1, class F>
  void zipWith(
      at::ArrayRef<T0> list_a,
      at::ArrayRef<T1> list_b,
      F action) const {
    auto it_a = list_a.begin();
    auto it_b = list_b.begin();

    if (list_a.size() != list_b.size()) {
      AT_ERROR("Pretty printer expected 2 lists of same size");
    }

    for (; it_a != list_a.end(); ++it_a, ++it_b) {
      action(*it_a, *it_b);
    }
  }

  void printValueList(std::ostream& stmt, at::ArrayRef<Value*> list, const char* begin = "", const char* end = "") {
    stmt << begin;
    auto delimiter = "";
    for (auto* value : list) {
      stmt << delimiter;
      stmt << useOf(value);
      delimiter = ", ";
    }
    stmt << end;
  }

  void printAssignment(
      at::ArrayRef<Value*> lhs,
      at::ArrayRef<Value*> rhs) {
    if(lhs.size() > 0) {
      indent();
      printValueList(out, lhs);
      out << " = ";
      printValueList(out, rhs);
      out << "\n";
    }
  }

  void printIf(IfView stmt) {
    assignValuesToTheirUniqueNames(stmt.outputs());
    indent() << "if " << useOf(stmt.cond()) << ":\n";
    {
      auto guard = WithIndented();
      // Print node contents
      printBlock(stmt.thenBlock(), stmt.outputs().size() > 0);
      printAssignment(stmt.outputs(), stmt.thenOutputs());
    }
    indent() << "else:\n";
    {
      auto guard = WithIndented();
      printBlock(stmt.elseBlock(), stmt.outputs().size() > 0);
      printAssignment(stmt.outputs(), stmt.elseOutputs());
    }
  }

  // our way of encoding loops makes them difficult to turn back into python syntax.
  // we have to check properties of the condition and trip count inputs to
  // figure out which one it initially was
  static bool shouldEmitAsForLoop(LoopView stmt) {
      auto trip_count = toIValue(stmt.maxTripCount());
      auto cond_input = toIValue(stmt.inputCond());
      auto cond_next = toIValue(stmt.nextCond());

      bool condition_is_always_true = cond_input && cond_input->toBool() && cond_next &&
        cond_next->toBool();
      bool trip_count_is_specified = !trip_count || // trip is not a constant
          trip_count->toInt() != std::numeric_limits<int64_t>::max() || // it is a constant but not the default one
          stmt.currentTripCount()->uses().size() > 0; // it is actually being used in the body.

      if (condition_is_always_true) {
        // if the trip count was not specified this was a user-written while True:
        return trip_count_is_specified;
      } else {
        // this must be a while loop, but check that there isn't _also_ a trip count
        if (trip_count_is_specified) {
          throw script::ErrorReport(stmt.node()->getSourceLocation())
              << "loop cannot be printed as python because it has gone through an optimization "
              << "that combined while and for loops. File a bug.";
        }
        return false;
      }
  }

  void printLoop(LoopView stmt) {

    // Loop carried dependencies are handled by assigning their initial
    // values to the node->outputs() before the loop,
    // and assign node->outputs() to the new values at the end of each trip.


    bool emit_as_for_loop = shouldEmitAsForLoop(stmt);

    assignValuesToTheirUniqueNames(stmt.carriedOutputs());
    // Add aliases for loop-carried dependencies
    zipWith(
        stmt.bodyCarriedInputs(), // Start at 1 to ignore trip count
        stmt.carriedOutputs(),
        [&](Value* block_input, Value* node_output) {
          assignValue(block_input, node_output);
        });

    // Print initial assignments of loop node outputs = loop node inputs
    printAssignment(stmt.carriedOutputs(), stmt.carriedInputs());

    assignValuesToTheirUniqueNames(stmt.currentTripCount());
    // Loop header
    if (emit_as_for_loop) {
      indent();
      out << "for " << useOf(stmt.currentTripCount()) << " in range("
          << useOf(stmt.maxTripCount()) << "):\n";
    } else {
      // note: trip_count_in_block is unused because this is a while loop,
      // so we reuse the Value* as a stand-in for the loop condition
      printAssignment(stmt.currentTripCount(), stmt.inputCond());
      indent();
      out << "while " << useOf(stmt.currentTripCount()) << ":\n";
    }
    // Loop body
    {
      ResourceGuard indent = WithIndented();
      // Update block outputs to block inputs for next loop iteration
      // skip the assignment to the new condition in for loops because
      // the condition is always True
      size_t offset = emit_as_for_loop ? 1 : 0;
      auto body_block = stmt.bodyBlock();
      ArrayRef<Value*> loop_carried_block_inputs = body_block->inputs().slice(offset);
      printBlock(body_block, loop_carried_block_inputs.size() > 0);
      printAssignment(loop_carried_block_inputs, body_block->outputs().slice(offset));
    }
  }

  void printNode(Node* node, bool print_const) {
    if (!print_const && isConstantLike(node))
      return;
    switch (node->kind()) {
      case prim::Return:
        if (node->inputs().size() > 0) {
          indent();
          out << "return ";
          printValueList(out, node->inputs());
          out << "\n";
        }
        break;
      case prim::Loop:
        printLoop(LoopView(node));
        break;
      case prim::If:
        printIf(IfView(node));
        break;
      case prim::TupleUnpack:
      case prim::ListUnpack:
        assignValuesToTheirUniqueNames(node->outputs());
        indent();
        // TupleUnpack(unpacked) turns into an assignment op that forces
        // the unpack to be inserted when parsed back in:
        // a, b, = unpacked
        // a, = unpacked # trailing comma forces an unpack to happen
        if (node->outputs().size() > 0) {
          printValueList(out, node->outputs(), "", ", = ");
        }
        out << useOf(node->input()) << "\n";
        break;
      default:

        std::stringstream ss;
        printRHS(ss, node);

        // this node is safe to inline, so assign the output value
        // to that expression directly
        // guard against really long lines
        if (output_inline_.count(node) > 0 && ss.str().size() + level * 2 < 40) {
          assignValue(node->output(), ss.str());
          return;
        }
        assignValuesToTheirUniqueNames(node->outputs());
        indent();
        // Print outputs
        if (node->outputs().size() > 0) {
          printValueList(out, node->outputs());
          out << " = ";
        }
        out << ss.str() << "\n";
    }
  }

  void printMaybeAnnotatedConstantList(
      std::ostream& stmt,
      const char* the_type,
      size_t list_size,
      IValue the_list) {
    if(list_size == 0) {
      stmt << "annotate(List[" << the_type << "], [])";
    } else {
      stmt << the_list;
    }
  }

  void printConstant(std::ostream& stmt, IValue v) {
    if(v.isTensor()) {
      stmt << "CONSTANTS.c" << getOrAddTensorConstant(v.toTensor());
    } else if(v.isString()) {
      printQuotedString(stmt, v.toStringRef());
    } else if(v.isDevice()) {
      std::stringstream ss;
      ss << v.toDevice();
      stmt << "torch.device(";
      printQuotedString(stmt, ss.str());
      stmt << ")";
    } else if(v.isTensorList()) {
      stmt << "[";
      const char* delim = "";
      for(auto t : v.toTensorListRef()) {
        stmt << delim << "CONSTANTS.c" << getOrAddTensorConstant(t);
        delim = ", ";
      }
      stmt << "]";
    } else if(v.isBoolList()) {
      printMaybeAnnotatedConstantList(stmt, "bool", v.toBoolListRef().size(), v);
    } else if(v.isIntList()) {
      printMaybeAnnotatedConstantList(stmt, "int", v.toIntListRef().size(), v);
    } else if(v.isDoubleList()) {
      printMaybeAnnotatedConstantList(stmt, "float", v.toDoubleListRef().size(), v);
    } else {
      stmt << v;
    }
  }

  // Prints the RHS value of a Node, e.g. `aten.add(x, y)`
  void printRHS(std::ostream& stmt, Node* node) {
    switch(node->kind()) {
      case PythonOp::Kind: {
        auto value = static_cast<const PythonOp*>(node);
        if (enforce_importable_) {
          throw script::ErrorReport(node->getSourceLocation())
              << "could not export python function call " << value->name()
              << ". Remove calls to python functions before export.";
        }

        stmt << "^" << value->name();
        value->writeScalars(stmt);
        printValueList(stmt, node->inputs(), "(", ")");
      } break;
      case prim::Constant: {
        IValue v = toIValue(node->output()).value();
        printConstant(stmt, v);
      } break;
      case prim::NoneGenerator:
      case prim::Undefined:
      case prim::None: {
        if (node->output()->type()->isSubtypeOf(NoneType::get())) {
          stmt << "None";
          break;
        }
        // XXX - we'd like to just print None in these circumstances
        // but implicit conversions from None to Tensor/Generator
        // are not always considered. E.g. if they are being put into a list.
        // Fixing this depends on removing specializations for Optional[Tensor]
        // an Optional[Generator] and universally using None.

        // XXX - when None has an Optional[T] type, we must ensure that type
        // can be recovered on parsing. It cannot be recovered if it will be
        // matched to schema with free variables. If it is used only in places where
        // there is schema and the scheme has no free variables, then we can
        // recover it without annotation. Otherwise, we annotate None with the right
        // optional type
        const auto& uses = node->output()->uses();
        bool all_usable_schema =
            std::all_of(uses.begin(), uses.end(), [](const Use& u) {
              if (auto schema = u.user->maybeSchema()) {
                if (u.offset >= schema->arguments().size()) {
                  return false;
                }
                return !schema->arguments()
                    .at(u.offset)
                    .type()
                    ->hasFreeVariables();
              }
              return false;
            });

        if (all_usable_schema) {
          stmt << "None";
        } else {
          stmt << "annotate(" << node->output()->type()->python_str() << ", None)";
        }
      } break;
      case prim::TensorToNum: {
        if (node->output()->type()->isSubtypeOf(IntType::get())) {
          printValueList(stmt, node->inputs(), "int(", ")");
        } else {
          JIT_ASSERT(node->output()->type()->isSubtypeOf(FloatType::get()));
          printValueList(stmt, node->inputs(), "float(", ")");
        }
      } break;
      case prim::ImplicitTensorToNum: {
        stmt << "annotate(" << node->output()->type()->python_str() << ", "
             << useOf(node->input()) << ")";
      } break;
      case prim::FloatToInt: {
        printValueList(stmt, node->inputs(), "int(", ")");
      } break;
      case prim::StringToFloat:
      case prim::IntToFloat: {
        printValueList(stmt, node->inputs(), "float(", ")");
      } break;
      case prim::TensorToBool: {
        printValueList(stmt, node->inputs(), "bool(", ")");
      } break;
      case prim::Print: {
        printValueList(stmt, node->inputs(), "print(",")");
      } break;
      case prim::TupleConstruct: {
        printValueList(
            stmt, node->inputs(), "(", node->inputs().size() == 1 ? ",)" : ")");
      } break;
      case prim::TupleIndex: {
        stmt << "(" << useOf(node->input()) << ")[" << node->i(attr::index) << "]";
      } break;
      case prim::TupleSlice: {
        stmt << "(" << useOf(node->input()) << ")[" << node->i(attr::beg) << ":"
             << node->i(attr::end) << "]";
      } break;
      case prim::ListConstruct: {
        // when the list is empty and is not a list of tensors,
        // we need to annotate it, otherwise it won't be possible
        // to infer the type on import
        if (node->inputs().size() == 0 &&
            !node->output()->type()->isSubtypeOf(DynamicType::get())) {
          stmt << "annotate(" << node->output()->type()->python_str() << ", [])";
        } else {
          printValueList(stmt, node->inputs(), "[", "]");
        }
      } break;
      case prim::fork: {
        // the subgraph gets emitted as another function
        auto name = genMethodName("__forked_function");
        std::shared_ptr<Graph> graph = node->g(attr::Subgraph);
        worklist.emplace_back([graph, name, this] {
          printFunctionDefinition(*graph, name);
        });
        // and we put a call to fork which invokes that function.
        stmt << "fork(self." << name;
        for(Value* v : node->inputs()) {
          stmt << ", " << useOf(v);
        }
        stmt << ")";
      } break;
      default: {
        Symbol kind = node->kind();
        if (kind.is_aten()) {
          // special case aten -> torch because we want to rename
          // the aten namespace, but this change will take more time
          // doing it here ensures we do not have fix up archives later
          stmt << "torch." << kind.toUnqualString() << "(";
        } else {
          stmt << "ops." << kind.ns().toUnqualString() << "." << kind.toUnqualString() << "(";
        }
        const FunctionSchema& schema = node->schema();
        for (size_t i = 0; i < node->inputs().size(); ++i) {
            if (i > 0) {
              stmt << ", ";
            }
            auto v = useOf(node->inputs().at(i));
            // print the kwarg name if it is a kwarg only argument.
            if (i < schema.arguments().size()) {
              auto arg = schema.arguments().at(i);
              if (arg.kwarg_only()) {
                stmt << arg.name() << "=";
              }
            } else {
              // vararg functions like format can have extra arguments
              JIT_ASSERT(schema.is_vararg());
            }
            stmt << v;
        }
        stmt << ")";
      } break;
    }
  }

  std::ostream& printBlock(Block* root, bool block_has_other_statements) {
    // pythons weird 'pass' syntax creates a bunch of places where we have to check
    // if this block would be empty. But not everything in a block is a node.
    // Sometimes if, loop, and return statements will follow this block
    // and block_has_other_statements == true.
    if (!block_has_other_statements &&
        root->nodes().begin() == root->nodes().end()) {
      indent();
      out << "pass\n";
    }
    for (auto* node : root->nodes()) {
      printNode(node, /*print_const=*/false);
    }
    return out;
  }

  void printDefaultValue(std::ostream& stmt, IValue value) {
    if (value.isTensor() && !value.toTensor().defined()) {
      // XXX - because undefined tensors are not stored as None, we need special handling.
      // otherwise they get printed as CONSTANTS.c0 and then cannot be recreated because
      // constant nodes cannot have an undefined value in them.
      // The right solution is to make None of type Tensor actually be an IValue None.
      stmt << "None";
      return;
    }
    printConstant(stmt, value);
  }
  void printFunctionDefinition(
      Graph& graph,
      const std::string& name,
      const std::vector<c10::optional<IValue>> defaults = {},
      const std::vector<std::string>& param_names = {}) {

    used_names_.clear(); // each graph can reuse local names

    // we always print constants at the top of the function, in the order
    // in which they are used.
    std::vector<Node*> constants;
    buildConstantList(graph.block(), constants);

    // current graph is used to de-dup names within a single graph
    scanBlock(graph.block());

    // last param_names.size() arguments to the graph are parameters and not
    // actual inputs, we will print these as, e.g. self.foo.bar
    // while we print the true_inputs out as parameters
    auto true_inputs = graph.inputs().slice(0, graph.inputs().size() - param_names.size());
    auto param_names_it = param_names.begin();
    for(auto param : graph.inputs().slice(true_inputs.size())) {
      assignValue(param, *param_names_it++);
    }
    assignValuesToTheirUniqueNames(true_inputs);
    out << "def " << name << "(self";
    auto defaults_offset = defaults.begin();
    for (auto input : true_inputs) {
      out << ",\n    " << useOf(input) << ": " << input->type()->python_str();
      if (defaults_offset != defaults.end()) {
        const c10::optional<IValue>& def = *defaults_offset++;
        if (def) {
          out << "=";
          printDefaultValue(out, *def);
        }
      }
    }

    // have we use all the provided defaults?
    JIT_ASSERT(defaults_offset == defaults.end());

    out << ") -> " << resultType(graph)->python_str() << ":\n";
    {
      auto guard = WithIndented();
      // Print initial constant table (most are just inlined into their use,
      // but some like long strings do get emitted)
      for (Node* n : constants) {
        printNode(n, /*print_const=*/true);
      }
      // Print body
      printBlock(
          graph.block(), graph.block()->return_node()->inputs().size() > 0);
      printNode(graph.block()->return_node(), /*print_const=*/false);
    }
  }

 public:
  PythonPrintPass(
      std::ostream& out_,
      std::vector<at::Tensor>& tensor_table,
      bool enforce_importable)
      : out(out_), tensor_table_(tensor_table), enforce_importable_(enforce_importable) {}

  // TODO: we should consider forcing functions to return a single value
  // instead of handling this tuple logic both in the compiler and the printer
  TypePtr resultType(const Graph& graph) {
    if (graph.outputs().size() == 1) {
      return graph.outputs().at(0)->type();
    } else {
      return TupleType::create(
          fmap(graph.outputs(), [&](const Value* v) { return v->type(); }));
    }
  }

  void printFunction(
      Graph& graph,
      const std::string& name,
      const std::vector<c10::optional<IValue>>& defaults = {},
      const std::vector<std::string>& param_names = {}) {
    printFunctionDefinition(graph, name, defaults, param_names);
    while(!worklist.empty()) {
      out << "\n\n";
      auto work = worklist.back();
      worklist.pop_back();
      work();
    }
  }
  void printMethod(script::Method& method) {
    std::unordered_map<at::Tensor*, QualifiedNamePtr> parameter_names;;
    createTensorToParameterNameMap(method.owner(), QualifiedName::create("self"),  parameter_names);
    printMethod(method, parameter_names);
  }
  void printMethod(
      script::Method& method,
      const std::unordered_map<at::Tensor*, QualifiedNamePtr>&
          parameter_names) {
    std::vector<std::string> param_names = fmap(
        method.params(),
        [&](at::Tensor* slot) { return parameter_names.at(slot)->str(); });
    const std::string& name = method.name();
    Graph& graph = *method.graph();
    auto defaults = fmap(method.getSchema().arguments(), [](const Argument& arg) {
      return arg.default_value();
    });
    printFunction(graph, name, defaults, param_names);
  }
  void printModule(script::Module& module) {
    std::unordered_map<at::Tensor*, QualifiedNamePtr> parameter_names;;
    createTensorToParameterNameMap(module, QualifiedName::create("self"),  parameter_names);
    for(auto& method : module.get_methods()) {
      const std::string& name = method.value()->name();
      // we skip __forked_functions because they actually get inlined into their
      // callers, exporting them again will lead to more code generated on each export
      if (name.find("__forked_function") == 0) {
        continue;
      }
      printMethod(*method.value(), parameter_names);
    }
  }
};

TORCH_API void PythonPrint(std::ostream& out, const Graph& graph, std::vector<at::Tensor>& tensor_table, bool enforce_importable) {
  PythonPrintPass pp(out, tensor_table, enforce_importable);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  pp.printFunction(const_cast<Graph&>(graph), "graph");
}

TORCH_API void PythonPrint(std::ostream& out, const script::Method& method, std::vector<at::Tensor>& tensor_table, bool enforce_importable) {
  PythonPrintPass pp(out, tensor_table, enforce_importable);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  pp.printMethod(const_cast<script::Method&>(method));
}

TORCH_API void PythonPrint(std::ostream& out, const script::Module& module, std::vector<at::Tensor>& tensor_table, bool enforce_importable) {
  PythonPrintPass pp(out, tensor_table, enforce_importable);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  pp.printModule(const_cast<script::Module&>(module));
}

TORCH_API bool printerHasSpecialCaseFor(Symbol sym) {
  // WARNING: by adding a value to this set, you are asserting
  // that you have also added special handling of this symbol to
  // the printer above. Not adding handling will cause import and export
  // of modules with this new operator to fail. This is only required
  // for operators without schema. Prefer registering your operator with
  // schema to editing this list here. These cases should only be things
  // that require special handling because they do not fit normal schema
  const static std::unordered_set<Symbol> handled = {
    prim::BoolToTensor,
    prim::Constant,
    prim::TensorToBool,
    prim::FloatToInt,
    prim::fork,
    prim::IntToFloat,
    prim::ListConstruct,
    prim::ListUnpack,
    prim::None,
    prim::NoneGenerator,
    prim::Print,
    prim::PythonOp,
    prim::StringToFloat,
    prim::TupleConstruct,
    prim::TupleIndex,
    prim::TupleSlice,
    prim::TupleUnpack,
    prim::Undefined,
  };

  // WARNING: by adding a value to this set, you are asserting that your
  // primitive is only ever added during optimization and does not need
  // to be correctly printed for export (a process that happens before
  // optimization passes run)
  const static std::unordered_set<Symbol> unneeded = {
    onnx::Reshape, // only used in onnx
    onnx::Shape, // only used in onnx
    prim::AnyDefined, // temporarily inserted by autograd
    prim::AutogradAdd, // temporarily inserted by autograd
    prim::ConstantChunk, // optimization pass adds it
    prim::DifferentiableGraph, // optimization pass adds it
    prim::BroadcastSizes, // optimization pass (fuser) adds it
    prim::ChunkSizes, // optimization pass (fuser) adds it
    prim::Drop, // used in interpreter only
    prim::FusedConcat, // optimization pass adds it
    prim::FusionGroup, // optimization pass adds it
    prim::Load, // used in interpreter only
    prim::MMTreeReduce, // used as an optimization
    prim::MMBatchSide, // used as an optimization
    prim::Store, // used in interpreter only

  };

  return handled.count(sym) || unneeded.count(sym);
}

} // namespace jit
} // namespace torch
