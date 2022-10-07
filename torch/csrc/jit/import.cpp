#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/type_resolver_util.h>

#include "torch/csrc/jit/import.h"
#include "torch/csrc/jit/ir.h"
#include "torch/csrc/utils/functional.h"
#include "torch/csrc/jit/assertions.h"
#include "torch/csrc/jit/operator.h"
#include "torch/csrc/jit/import_method.h"


#include "caffe2/core/types.h"
#include "caffe2/proto/caffe2_pb.h"
#include "caffe2/proto/torch_pb.h"
#include "caffe2/serialize/inline_container.h"

#include <ATen/ATen.h>

#include <unordered_map>
#include <vector>
#include <string>
#include <fstream>

namespace torch { namespace jit {

namespace {

// this is a deserializer class which loads script modules from pt files. the
// content of the file is written using PyTorchStreamWriter, for details please
// check caffe2/serialize/inline_container.h. all the records except the last
// one are tensor data, and the last record is a serialized ModelProto, defined
// in caffe2/proto/torch.proto. ModelProto contains all the metadata of the
// model, and it is serialized as json.
class ScriptModuleDeserializer final {
 public:
  ScriptModuleDeserializer(const std::string& filename);

  ScriptModuleDeserializer(std::istream* is);

  void deserialize(ModuleLookup module_lookup,
      c10::optional<at::Device> device);

private:
 at::Tensor loadTensor(
     const torch::TensorDef& tensor_proto,
     std::unordered_map<std::string, at::Storage>& storageMap);

 void convertModule(const torch::ModuleDef& module_def);

 void loadTensorTable(torch::ModelDef* model_def);

 std::ifstream ifs_;
 PyTorchStreamReader reader_;
 // this is a hack to make sure the script module created in C++ is the
 // same as created in Python
 ModuleLookup moduleLookup_;
 c10::optional<at::Device> device_;
 std::vector<std::string> moduleStack_;

 std::vector<at::Tensor> tensor_table_;
};

ScriptModuleDeserializer::ScriptModuleDeserializer(const std::string& filename)
    : reader_(filename.c_str()) {
  // TODO appropriate support for mmap, right now still use stream reader
}

ScriptModuleDeserializer::ScriptModuleDeserializer(std::istream* is)
    : ifs_(), reader_(is) {}

void ScriptModuleDeserializer::deserialize(ModuleLookup module_lookup,
    c10::optional<at::Device> device) {
  torch::ModelDef model_def;
  at::DataPtr data_ptr;
  size_t data_size;
  std::tie(data_ptr, data_size) = reader_.getRecord("model.json");
  // NB: cannot use JsonStringToMessage, since fbcode's protobuf is too old
  // be consistent with JsonStringToMessage
  std::string url_prefix = "type.googleapis.com";
  std::unique_ptr<::google::protobuf::util::TypeResolver> resolver(
      ::google::protobuf::util::NewTypeResolverForDescriptorPool(
          url_prefix, model_def.GetDescriptor()->file()->pool()));
  std::string json_string = std::string(
      static_cast<char*>(data_ptr.get()),
      static_cast<char*>(data_ptr.get()) + data_size);
  std::string binary_string;
  auto convert_result = ::google::protobuf::util::JsonToBinaryString(
      resolver.get(),
      url_prefix + "/" + model_def.GetDescriptor()->full_name(),
      json_string,
      &binary_string);
  if (!convert_result.ok()) {
    std::stringstream ss;
    ss << convert_result;
    AT_ERROR(ss.str());
  }
  AT_ASSERTM(
      model_def.ParseFromString(binary_string),
      "JSON transcoder produced invalid protobuf output.");
  moduleLookup_ = module_lookup;
  device_ = device;

  const auto& module_def = model_def.main_module();
  loadTensorTable(&model_def);
  // TODO: this can be simplified when C++/Python interop lands,
  // and the submodules would be created as the same in either C++ or Python
  convertModule(module_def);
}

void ScriptModuleDeserializer::loadTensorTable(torch::ModelDef* model_def) {
  std::unordered_map<std::string, at::Storage> storageMap;
  for(const torch::TensorDef& tensor : model_def->tensors()) {
    tensor_table_.emplace_back(loadTensor(tensor, storageMap));
  }
}

at::Tensor ScriptModuleDeserializer::loadTensor(const torch::TensorDef& tensor_proto,
                std::unordered_map<std::string, at::Storage>& storageMap) {
  std::vector<int64_t> dims(tensor_proto.dims().begin(), tensor_proto.dims().end());
  std::vector<int64_t> strides(tensor_proto.strides().begin(), tensor_proto.strides().end());
  auto type = at::typeMetaToScalarType(
      caffe2::DataTypeToTypeMeta(tensor_proto.data_type()));
  const std::string& record_key = tensor_proto.data().key();
  AT_ASSERT(tensor_proto.has_device() && !tensor_proto.device().empty());
  at::Device device(tensor_proto.device());
  if (device_.has_value()) {
    // override the device, if user provides map_location
    device = device_.value();
  }

  auto storage_it = storageMap.find(record_key);
  if (storage_it == storageMap.end()) {
    at::DataPtr storage_ptr;
    uint64_t record_size;
    std::tie(storage_ptr, record_size) = reader_.getRecord(record_key);
    auto cpu_storage = at::Storage(
        at::CPU(type).typeMeta(),
        std::move(storage_ptr),
        record_size / at::CPU(type).typeMeta().itemsize(),
        nullptr); // NB: we didn't set any allocator for the tensor
    if (device.type() == at::DeviceType::CPU) {
      storage_it = storageMap.insert(std::make_pair(
            record_key, cpu_storage)).first;
    } else if (device.type() == at::DeviceType::CUDA) {
      at::Tensor cpu_tensor = at::CPU(type)._th_tensor(
          cpu_storage, tensor_proto.offset(), dims, strides);
      at::Storage cuda_storage = cpu_tensor.to(device,
          cpu_tensor.scalar_type()).storage();
      storage_it = storageMap.insert(std::make_pair(
            record_key, cuda_storage)).first;
    } else {
      AT_ERROR("supported devices include CPU and CUDA, however got ",
          at::DeviceTypeName(device.type(), false));
    }
  }
  if (storage_it->second.device().type() != device.type() ||
      (device.has_index() &&
       storage_it->second.device().index() != device.index())) {
    std::stringstream oss;
    oss << "storage previously was specified with device "
      << storage_it->second.device()
      << "but now is specified with device "
      << device << std::endl;
    AT_ERROR(oss.str());
  }

  at::Tensor result;
  if (device.type() == at::DeviceType::CPU) {
    result = at::CPU(type)._th_tensor(
        storage_it->second, tensor_proto.offset(), dims, strides);
  } else if (device.type() == at::DeviceType::CUDA) {
    result = at::CUDA(type)._th_tensor(
        storage_it->second, tensor_proto.offset(), dims, strides);
  }
  AT_ASSERT(result.defined());

  result = autograd::make_variable(result, tensor_proto.requires_grad());

  return result;
}

void ScriptModuleDeserializer::convertModule(
    const torch::ModuleDef& module_def) {
  std::shared_ptr<script::Module> module = moduleLookup_(moduleStack_);
  module->set_optimized(module_def.optimize());
  for (int i = 0; i < module_def.submodules_size(); ++i) {
    const torch::ModuleDef& sub_def = module_def.submodules(i);
    moduleStack_.emplace_back(sub_def.name());
    convertModule(sub_def);
    moduleStack_.pop_back();
  }
  for (int i = 0; i < module_def.parameters_size(); ++i) {
    const torch::ParameterDef& param_def = module_def.parameters(i);
    at::Tensor tensor = tensor_table_.at(param_def.tensor_id());
    module->register_parameter(
        param_def.name(), tensor, param_def.is_buffer());
  }
  if (module_def.has_torchscript_arena()) {
    at::DataPtr data;
    size_t size;
    std::tie(data, size) = reader_.getRecord(module_def.torchscript_arena().key());
    std::string data_str(static_cast<const char*>(data.get()), size);
    import_methods(module, data_str, tensor_table_);
  }
}

}  // namespace

void import_ir_module(
    ModuleLookup module_lookup,
    std::istream& in,
    c10::optional<at::Device> device) {
  ScriptModuleDeserializer deserializer(&in);
  deserializer.deserialize(module_lookup, device);
}

void import_ir_module(
    ModuleLookup module_lookup,
    const std::string& filename,
    c10::optional<at::Device> device) {
  ScriptModuleDeserializer deserializer(filename);
  deserializer.deserialize(module_lookup, device);
}

std::shared_ptr<script::Module> load(std::istream& in,
    c10::optional<at::Device> device) {
  auto module = std::make_shared<script::Module>();

  auto module_lookup = [&](const std::vector<std::string>& qualified_name) {
    std::shared_ptr<script::Module> curr = module;
    for (const auto& name : qualified_name) {
      if (curr->find_module(name) == nullptr) {
        curr->register_module(name, std::make_shared<script::Module>());
      }
      curr = curr->get_module(name);
    }
    return curr;
  };

  ScriptModuleDeserializer deserializer(&in);
  deserializer.deserialize(module_lookup, device);

  return module;
}

std::shared_ptr<script::Module> load(const std::string& filename,
    c10::optional<at::Device> device) {
  std::ifstream in(filename, std::ios_base::binary);

  AT_CHECK(! in.fail(), "load: could not open file ", filename);

  auto module = load(in, device);

  return module;
}

}}
