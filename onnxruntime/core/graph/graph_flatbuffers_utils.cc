// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "graph_flatbuffers_utils.h"
#include "core/framework/tensorprotoutils.h"

using namespace ONNX_NAMESPACE;
using namespace ::onnxruntime::common;
using namespace ::onnxruntime::experimental;

namespace onnxruntime {
namespace experimental {
namespace utils {

static flatbuffers::Offset<fbs::Dimension> GetTensorDimensionOrtFormat(
    flatbuffers::FlatBufferBuilder& builder,
    const TensorShapeProto_Dimension& tensor_shape_dim) {
  auto denotation = builder.CreateString(tensor_shape_dim.denotation());
  flatbuffers::Offset<fbs::DimensionValue> dim_val = 0;
  if (tensor_shape_dim.has_dim_param()) {
    dim_val = fbs::CreateDimensionValueDirect(builder, fbs::DimensionValueType_PARAM, 0, tensor_shape_dim.dim_param().c_str());
  } else if (tensor_shape_dim.has_dim_value()) {
    dim_val = fbs::CreateDimensionValueDirect(builder, fbs::DimensionValueType_VALUE, tensor_shape_dim.dim_value());
  }

  return fbs::CreateDimension(builder, dim_val, denotation);
}

static Status GetTensorShapeOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                                      const TensorShapeProto& tensor_shape_proto,
                                      flatbuffers::Offset<fbs::Shape>& fbs_shape) {
  std::vector<flatbuffers::Offset<fbs::Dimension>> dim;
  dim.reserve(tensor_shape_proto.dim_size());
  for (const auto& d : tensor_shape_proto.dim()) {
    auto fbs_d = GetTensorDimensionOrtFormat(builder, d);
    dim.push_back(fbs_d);
  }
  fbs_shape = fbs::CreateShapeDirect(builder, &dim);
  return Status::OK();
}

static Status GetTensorTypeAndShapeOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                                             const TypeProto_Tensor& tensor_type_proto,
                                             flatbuffers::Offset<fbs::TensorTypeAndShape>& fbs_tensor_type) {
  flatbuffers::Offset<fbs::Shape> shape;
  ORT_RETURN_IF_ERROR(GetTensorShapeOrtFormat(builder, tensor_type_proto.shape(), shape));
  fbs_tensor_type = fbs::CreateTensorTypeAndShape(
      builder, static_cast<fbs::TensorDataType>(tensor_type_proto.elem_type()), shape);
  return Status::OK();
}

static Status GetTypeInfoOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                                   const TypeProto& type_proto,
                                   flatbuffers::Offset<fbs::TypeInfo>& fbs_type_info) {
  auto denotation = builder.CreateString(type_proto.denotation());
  auto value_type = fbs::TypeInfoValue_tensor_type;
  flatbuffers::Offset<void> value;
  if (type_proto.has_tensor_type()) {
    value_type = fbs::TypeInfoValue_tensor_type;
    flatbuffers::Offset<fbs::TensorTypeAndShape> tensor_type;
    ORT_RETURN_IF_ERROR(
        GetTensorTypeAndShapeOrtFormat(builder, type_proto.tensor_type(), tensor_type));
    value = tensor_type.Union();
  } else {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "We only support tensor type for now");
  }

  fbs::TypeInfoBuilder tb(builder);
  tb.add_denotation(denotation);
  tb.add_value_type(value_type);
  tb.add_value(value);
  fbs_type_info = tb.Finish();
  return Status::OK();
}

Status GetValueInfoOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                             const ValueInfoProto& value_info_proto,
                             flatbuffers::Offset<fbs::ValueInfo>& fbs_value_info) {
  auto name = builder.CreateString(value_info_proto.name());
  auto doc_string = builder.CreateString(value_info_proto.doc_string());
  flatbuffers::Offset<fbs::TypeInfo> type_info;
  if (value_info_proto.has_type()) {
    ORT_RETURN_IF_ERROR(
        GetTypeInfoOrtFormat(builder, value_info_proto.type(), type_info));
  } else {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "value_info_proto has no type");
  }

  fbs::ValueInfoBuilder vb(builder);
  vb.add_name(name);
  vb.add_doc_string(doc_string);
  vb.add_type(type_info);
  fbs_value_info = vb.Finish();
  return Status::OK();
}

Status GetInitializerOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                               const TensorProto& initializer,
                               flatbuffers::Offset<fbs::Tensor>& fbs_tensor) {
  auto name = builder.CreateString(initializer.name());
  auto doc_string = builder.CreateString(initializer.doc_string());
  std::vector<int64_t> dims_data(initializer.dims().size());
  std::copy(initializer.dims().cbegin(), initializer.dims().cend(), dims_data.begin());
  auto dims = builder.CreateVector(dims_data);
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> string_data;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> raw_data;

  auto src_type = initializer.data_type();
  bool has_string_data = src_type == ONNX_NAMESPACE::TensorProto_DataType_STRING;
  if (has_string_data) {
    std::vector<std::string> string_data_vec(initializer.string_data().size());
    std::copy(initializer.string_data().cbegin(), initializer.string_data().cend(), string_data_vec.begin());
    string_data = builder.CreateVectorOfStrings(string_data_vec);
  } else {
    std::unique_ptr<uint8_t[]> unpacked_tensor;
    size_t tensor_byte_size;
    ORT_RETURN_IF_ERROR(
        onnxruntime::utils::UnpackInitializerData(initializer, unpacked_tensor, tensor_byte_size));
    raw_data = builder.CreateVector(unpacked_tensor.get(), tensor_byte_size);
  }

  fbs::TensorBuilder tb(builder);
  tb.add_name(name);
  tb.add_doc_string(doc_string);
  tb.add_dims(dims);
  tb.add_data_type(static_cast<fbs::TensorDataType>(src_type));
  if (has_string_data)
    tb.add_string_data(string_data);
  else
    tb.add_raw_data(raw_data);
  fbs_tensor = tb.Finish();
  return Status::OK();
}

#define GET_FBS_ATTR(BUILDER, TYPE, DATA_NAME, DATA) \
  fbs::AttributeBuilder attr_builder(BUILDER);       \
  attr_builder.add_name(name);                       \
  attr_builder.add_doc_string(doc_string);           \
  attr_builder.add_type(TYPE);                       \
  attr_builder.add_##DATA_NAME(DATA);                \
  fbs_attr = attr_builder.Finish();                  \
  return Status::OK();

#define GET_DATA_VEC(TYPE, NAME, SRC_DATA) \
  std::vector<TYPE> NAME(SRC_DATA.size()); \
  std::copy(SRC_DATA.cbegin(), SRC_DATA.cend(), NAME.begin());

Status GetAttributeOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                             const AttributeProto& attr_proto,
                             flatbuffers::Offset<fbs::Attribute>& fbs_attr,
                             const onnxruntime::Graph* graph) {
  auto name = builder.CreateString(attr_proto.name());
  auto doc_string = builder.CreateString(attr_proto.doc_string());
  auto type = static_cast<fbs::AttributeType>(attr_proto.type());
  switch (type) {
    case fbs::AttributeType_FLOAT: {
      GET_FBS_ATTR(builder, type, f, attr_proto.f());
    } break;
    case fbs::AttributeType_INT: {
      GET_FBS_ATTR(builder, type, i, attr_proto.i());
    } break;
    case fbs::AttributeType_STRING: {
      auto s = builder.CreateString(attr_proto.s());
      GET_FBS_ATTR(builder, type, s, s);
    } break;
    case fbs::AttributeType_TENSOR: {
      flatbuffers::Offset<fbs::Tensor> fbs_tensor;
      ORT_RETURN_IF_ERROR(
          experimental::utils::GetInitializerOrtFormat(builder, attr_proto.t(), fbs_tensor));
      GET_FBS_ATTR(builder, type, t, fbs_tensor);
    } break;
    case fbs::AttributeType_GRAPH: {
      ORT_RETURN_IF_NOT(nullptr != graph, "GetAttributeOrtFormat, graph is null");
      flatbuffers::Offset<fbs::Graph> fbs_graph;
      ORT_RETURN_IF_ERROR(graph->SaveToOrtFormat(builder, fbs_graph));
      GET_FBS_ATTR(builder, type, g, fbs_graph);
    } break;
    case fbs::AttributeType_FLOATS: {
      GET_DATA_VEC(float, floats_vec_, attr_proto.floats());
      auto floats = builder.CreateVector(floats_vec_);
      GET_FBS_ATTR(builder, type, floats, floats);
    } break;
    case fbs::AttributeType_INTS: {
      GET_DATA_VEC(int64_t, ints_vec_, attr_proto.ints());
      auto ints = builder.CreateVector(ints_vec_);
      GET_FBS_ATTR(builder, type, ints, ints);
    } break;
    case fbs::AttributeType_STRINGS: {
      GET_DATA_VEC(std::string, strings_vec_, attr_proto.strings());
      auto strings = builder.CreateVectorOfStrings(strings_vec_);
      GET_FBS_ATTR(builder, type, strings, strings);
    } break;
    case fbs::AttributeType_TENSORS: {
      std::vector<flatbuffers::Offset<fbs::Tensor>> fbs_tensors_vec;
      fbs_tensors_vec.reserve(attr_proto.tensors().size());
      for (const auto& tensor : attr_proto.tensors()) {
        flatbuffers::Offset<fbs::Tensor> fbs_tensor;
        ORT_RETURN_IF_ERROR(
            experimental::utils::GetInitializerOrtFormat(builder, tensor, fbs_tensor));
        fbs_tensors_vec.push_back(fbs_tensor);
      }
      auto tensors = builder.CreateVector(fbs_tensors_vec);
      GET_FBS_ATTR(builder, type, tensors, tensors);
    } break;
    default:
      break;
  }

  return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "GetAttributeOrtFormat - Unsupported type: ", type);
}

#undef GET_FBS_ATTR
#undef GET_DATA_VEC

#define FBS_SET_STR_VAL(OBJ, FUNC, FBS_STR) \
  if (FBS_STR) {                            \
    OBJ.FUNC(FBS_STR->str());               \
  }

Status LoadInitializerOrtFormat(const fbs::Tensor& fbs_tensor,
                                TensorProto& initializer) {
  initializer.Clear();

  FBS_SET_STR_VAL(initializer, set_name, fbs_tensor.name());
  FBS_SET_STR_VAL(initializer, set_doc_string, fbs_tensor.doc_string());

  auto fbs_dims = fbs_tensor.dims();
  ORT_RETURN_IF_NOT(nullptr != fbs_dims, "fbs_dims cannot be null");
  initializer.mutable_dims()->Add(fbs_dims->cbegin(), fbs_dims->cend());

  auto fbs_data_type = fbs_tensor.data_type();
  initializer.set_data_type(fbs_data_type);
  if (fbs_data_type == fbs::TensorDataType_STRING) {
    auto fbs_str_data = fbs_tensor.string_data();
    ORT_RETURN_IF_NOT(nullptr != fbs_str_data, "fbs_str_data cannot be null");
    auto mutable_str_data = initializer.mutable_string_data();
    mutable_str_data->Reserve(fbs_str_data->size());
    for (const auto& fbs_str : *fbs_str_data) {
      mutable_str_data->Add(fbs_str->str());
    }
  } else {
    auto fbs_raw_data = fbs_tensor.raw_data();
    // Should we throw in this case? or just leave the raw_data to be empty?
    ORT_RETURN_IF_NOT(nullptr != fbs_raw_data, "fbs_raw_data cannot be null");
    // fbs_raw_data is uint8_t vector, so the size is byte size
    initializer.set_raw_data(fbs_raw_data->Data(), fbs_raw_data->size());
  }

  return Status::OK();
}

static Status LoadTensorDimensionOrtFormat(const fbs::Dimension& fbs_dim,
                                           TensorShapeProto_Dimension& dim) {
  dim.Clear();
  FBS_SET_STR_VAL(dim, set_denotation, fbs_dim.denotation());
  auto fbs_dim_val = fbs_dim.value();
  if (fbs_dim_val) {
    auto type = fbs_dim_val->dim_type();
    if (type == fbs::DimensionValueType_VALUE)
      dim.set_dim_value(fbs_dim_val->dim_value());
    else if (type == fbs::DimensionValueType_PARAM) {
      auto fbs_dim_param = fbs_dim_val->dim_param();
      ORT_RETURN_IF_NOT(nullptr != fbs_dim_param, "fbs_dim_param cannot be null");
      dim.set_dim_param(fbs_dim_param->str());
    }
  }
  return Status::OK();
}

static Status LoadTensorTypeAndShapeOrtFormat(const fbs::TensorTypeAndShape& fbs_tensor_type,
                                              TypeProto_Tensor& tensor_type_proto) {
  tensor_type_proto.Clear();
  tensor_type_proto.set_elem_type(fbs_tensor_type.elem_type());
  auto fbs_shape = fbs_tensor_type.shape();
  if (fbs_shape) {
    auto fbs_dims = fbs_shape->dim();
    if (fbs_dims) {
      auto dims = tensor_type_proto.mutable_shape()->mutable_dim();
      dims->Reserve(fbs_dims->size());
      for (const auto fbs_dim : *fbs_dims) {
        ORT_RETURN_IF_NOT(fbs_dim, "fbs_dim cannot be null");
        TensorShapeProto_Dimension dim;
        ORT_RETURN_IF_ERROR(LoadTensorDimensionOrtFormat(*fbs_dim, *dims->Add()));
      }
    }
  }
  return Status::OK();
}

static Status LoadTypeInfoOrtFormat(const fbs::TypeInfo& fbs_type_info,
                                    TypeProto& type_proto) {
  type_proto.Clear();
  FBS_SET_STR_VAL(type_proto, set_denotation, fbs_type_info.denotation());
  auto value_type = fbs_type_info.value_type();
  if (value_type == fbs::TypeInfoValue_tensor_type) {
    auto fbs_tensor_type = fbs_type_info.value_as_tensor_type();
    ORT_RETURN_IF_NOT(nullptr != fbs_tensor_type, "fbs_tensor_type cannot be null");
    ORT_RETURN_IF_ERROR(LoadTensorTypeAndShapeOrtFormat(*fbs_tensor_type, *type_proto.mutable_tensor_type()));
  } else {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Type:", value_type, " is not supported for now");
  }

  return Status::OK();
}

Status LoadValueInfoOrtFormat(const fbs::ValueInfo& fbs_value_info,
                              ONNX_NAMESPACE::ValueInfoProto& value_info_proto) {
  value_info_proto.Clear();

  FBS_SET_STR_VAL(value_info_proto, set_name, fbs_value_info.name());
  FBS_SET_STR_VAL(value_info_proto, set_doc_string, fbs_value_info.doc_string());

  auto fbs_type_info = fbs_value_info.type();
  ORT_RETURN_IF_NOT(nullptr != fbs_type_info, "fbs_type_info cannot be null");
  ORT_RETURN_IF_ERROR(LoadTypeInfoOrtFormat(*fbs_type_info, *value_info_proto.mutable_type()));

  return Status::OK();
}

onnxruntime::common::Status LoadAttributeOrtFormat(const fbs::Attribute& fbs_attr,
                                                   ONNX_NAMESPACE::AttributeProto& attr_proto,
                                                   std::unique_ptr<onnxruntime::Graph>& sub_graph,
                                                   Graph& graph, Node& node,
                                                   const logging::Logger& logger) {
  attr_proto.Clear();
  FBS_SET_STR_VAL(attr_proto, set_name, fbs_attr.name());
  FBS_SET_STR_VAL(attr_proto, set_doc_string, fbs_attr.doc_string());
  auto type = static_cast<AttributeProto_AttributeType>(fbs_attr.type());
  attr_proto.set_type(type);
  switch (type) {
    case AttributeProto_AttributeType_FLOAT: {
      attr_proto.set_f(fbs_attr.f());
    } break;
    case AttributeProto_AttributeType_INT: {
      attr_proto.set_i(fbs_attr.i());
    } break;
    case AttributeProto_AttributeType_STRING: {
      auto fbs_str = fbs_attr.s();
      ORT_RETURN_IF_NOT(nullptr != fbs_str, "fbs_str cannot be null");
      attr_proto.set_s(fbs_str->str());
    } break;
    case AttributeProto_AttributeType_TENSOR: {
      auto fbs_tensor = fbs_attr.t();
      ORT_RETURN_IF_NOT(nullptr != fbs_tensor, "fbs_tensor cannot be null");
      ORT_RETURN_IF_ERROR(LoadInitializerOrtFormat(*fbs_tensor, *attr_proto.mutable_t()));
    } break;
    case AttributeProto_AttributeType_GRAPH: {
      auto fbs_graph = fbs_attr.g();
      ORT_RETURN_IF_NOT(nullptr != fbs_graph, "fbs_graph cannot be null");
      attr_proto.mutable_g()->set_name("Empty graph proto from deserialization of ORT format model");
      ORT_RETURN_IF_ERROR(Graph::LoadFromOrtFormat(*fbs_graph, graph, node, logger, sub_graph));
    } break;
    case AttributeProto_AttributeType_FLOATS: {
      auto fbs_floats = fbs_attr.floats();
      ORT_RETURN_IF_NOT(nullptr != fbs_floats, "fbs_floats cannot be null");
      auto floats = attr_proto.mutable_floats();
      floats->Reserve(fbs_floats->size());
      floats->Add(fbs_floats->cbegin(), fbs_floats->cend());
    } break;
    case AttributeProto_AttributeType_INTS: {
      auto fbs_ints = fbs_attr.ints();
      ORT_RETURN_IF_NOT(nullptr != fbs_ints, "fbs_ints cannot be null");
      auto* ints = attr_proto.mutable_ints();
      ints->Reserve(fbs_ints->size());
      ints->Add(fbs_ints->cbegin(), fbs_ints->cend());
    } break;
    case AttributeProto_AttributeType_STRINGS: {
      auto fbs_strings = fbs_attr.strings();
      ORT_RETURN_IF_NOT(nullptr != fbs_strings, "fbs_strings cannot be null");
      auto* strings = attr_proto.mutable_strings();
      strings->Reserve(fbs_strings->size());
      for (const auto* fbs_str : *fbs_strings) {
        ORT_RETURN_IF_NOT(nullptr != fbs_str, "fbs_str cannot be null");
        strings->Add(fbs_str->str());
      }
    } break;
    case AttributeProto_AttributeType_TENSORS: {
      auto fbs_tensors = fbs_attr.tensors();
      ORT_RETURN_IF_NOT(nullptr != fbs_tensors, "fbs_tensors cannot be null");
      auto* tensors = attr_proto.mutable_tensors();
      tensors->Reserve(fbs_tensors->size());
      for (const auto* fbs_tensor : *fbs_tensors) {
        ORT_RETURN_IF_NOT(nullptr != fbs_tensor, "fbs_str cannot be null");
        ORT_RETURN_IF_NOT(nullptr != fbs_tensor, "fbs_tensor cannot be null");
        ORT_RETURN_IF_ERROR(LoadInitializerOrtFormat(*fbs_tensor, *tensors->Add()));
      }
    } break;

    default:
      break;
  }

  return Status::OK();
}

}  // namespace utils
}  // namespace experimental
}  // namespace onnxruntime