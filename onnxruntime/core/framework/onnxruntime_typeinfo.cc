// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

//this file contains implementations of the C API

#include "onnxruntime_typeinfo.h"
#include <cassert>
#include "core/framework/tensor.h"
#include "core/graph/onnx_protobuf.h"

using onnxruntime::DataTypeImpl;
using onnxruntime::MLFloat16;
using onnxruntime::Tensor;
using onnxruntime::TensorShape;

OrtTypeInfo::OrtTypeInfo(OrtType type1, void* data1) noexcept : type(type1), data(data1) {
}

OrtTypeInfo::~OrtTypeInfo() {
  assert(ref_count == 0);
  OrtReleaseObject(data);
}

ORT_API(const struct OrtTensorTypeAndShapeInfo*, OrtCastTypeInfoToTensorInfo, _In_ struct OrtTypeInfo* input) {
  return input->type == ORT_TYPE_TENSOR ? reinterpret_cast<const struct OrtTensorTypeAndShapeInfo*>(input->data) : nullptr;
}

ONNXStatus* GetTensorShapeAndType(const TensorShape* shape, const onnxruntime::DataTypeImpl* tensor_data_type, OrtTensorTypeAndShapeInfo** out);

ONNXStatus* OrtTypeInfo::FromDataTypeImpl(const onnxruntime::DataTypeImpl* input, const TensorShape* shape, const onnxruntime::DataTypeImpl* tensor_data_type, OrtTypeInfo** out) {
  if (input == nullptr) {
    *out = new OrtTypeInfo(ORT_TYPE_UNKNOWN, nullptr);
    return nullptr;
  }
  if (input == DataTypeImpl::GetType<Tensor>()) {
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (tensor_data_type != nullptr) {
      ONNXStatus* st = GetTensorShapeAndType(shape, tensor_data_type, &info);
      if (st != nullptr) return st;
    }
    *out = new OrtTypeInfo(ORT_TYPE_TENSOR, info);
    return nullptr;
  }
  if (input == DataTypeImpl::GetType<onnxruntime::MapStringToString>() || input == DataTypeImpl::GetType<onnxruntime::MapStringToInt64>() || input == DataTypeImpl::GetType<onnxruntime::MapStringToFloat>() || input == DataTypeImpl::GetType<onnxruntime::MapStringToDouble>() || input == DataTypeImpl::GetType<onnxruntime::MapInt64ToString>() || input == DataTypeImpl::GetType<onnxruntime::MapInt64ToInt64>() || input == DataTypeImpl::GetType<onnxruntime::MapInt64ToFloat>() || input == DataTypeImpl::GetType<onnxruntime::MapInt64ToDouble>()) {
    *out = new OrtTypeInfo(ORT_TYPE_MAP, nullptr);
    return nullptr;
  }
  if (input == DataTypeImpl::GetType<onnxruntime::VectorString>() || input == DataTypeImpl::GetType<onnxruntime::VectorFloat>() || input == DataTypeImpl::GetType<onnxruntime::VectorInt64>() || input == DataTypeImpl::GetType<onnxruntime::VectorDouble>() || input == DataTypeImpl::GetType<onnxruntime::VectorMapStringToFloat>() || input == DataTypeImpl::GetType<onnxruntime::VectorMapInt64ToFloat>()) {
    *out = new OrtTypeInfo(ORT_TYPE_SEQUENCE, nullptr);
    return nullptr;
  }
  return CreateONNXStatus(ORT_NOT_IMPLEMENTED, "not implemented");
}

const DataTypeImpl* ElementTypeFromProto(ONNX_NAMESPACE::TensorProto_DataType type) {
  switch (type) {
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT:
      return DataTypeImpl::GetType<float>();
    case ONNX_NAMESPACE::TensorProto_DataType_BOOL:
      return DataTypeImpl::GetType<bool>();
    case ONNX_NAMESPACE::TensorProto_DataType_INT32:
      return DataTypeImpl::GetType<int>();
    case ONNX_NAMESPACE::TensorProto_DataType_DOUBLE:
      return DataTypeImpl::GetType<double>();
    case ONNX_NAMESPACE::TensorProto_DataType_STRING:
      return DataTypeImpl::GetType<std::string>();
    case ONNX_NAMESPACE::TensorProto_DataType_INT8:
      return DataTypeImpl::GetType<int8_t>();
    case ONNX_NAMESPACE::TensorProto_DataType_UINT8:
      return DataTypeImpl::GetType<uint8_t>();
    case ONNX_NAMESPACE::TensorProto_DataType_UINT16:
      return DataTypeImpl::GetType<uint16_t>();
    case ONNX_NAMESPACE::TensorProto_DataType_INT16:
      return DataTypeImpl::GetType<int16_t>();
    case ONNX_NAMESPACE::TensorProto_DataType_INT64:
      return DataTypeImpl::GetType<int64_t>();
    case ONNX_NAMESPACE::TensorProto_DataType_UINT32:
      return DataTypeImpl::GetType<uint32_t>();
    case ONNX_NAMESPACE::TensorProto_DataType_UINT64:
      return DataTypeImpl::GetType<uint64_t>();
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT16:
      return DataTypeImpl::GetType<MLFloat16>();
    default:
      ORT_NOT_IMPLEMENTED(__FUNCTION__, ":tensor type ", type, " is not supported");
  }
}

ONNXStatus* OrtTypeInfo::FromDataTypeImpl(const onnx::TypeProto* input, OrtTypeInfo** out) {
  if (input->has_tensor_type()) {
    const ::onnx::TypeProto_Tensor& onnx_tensor_info = input->tensor_type();
    const DataTypeImpl* type = ElementTypeFromProto(onnx_tensor_info.elem_type());
    ONNXStatus* st;
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (onnx_tensor_info.has_shape()) {
      const ::onnx::TensorShapeProto& s = onnx_tensor_info.shape();
      std::vector<int64_t> shape_data(s.dim_size());
      for (int i = 0; i != s.dim_size(); ++i) {
        auto& t = s.dim(i);
        shape_data[i] = t.has_dim_value() ? t.dim_value() : -1;
      }
      st = GetTensorShapeAndType(reinterpret_cast<const TensorShape*>(&shape_data), type, &info);
    } else {
      st = GetTensorShapeAndType(nullptr, type, &info);
    }

    if (st != nullptr) return st;
    *out = new OrtTypeInfo(ORT_TYPE_TENSOR, info);
    return nullptr;
  }
  if (input->has_sequence_type()) {
    *out = new OrtTypeInfo(ORT_TYPE_SEQUENCE, nullptr);
    return nullptr;
  }
  if (input->has_map_type()) {
    *out = new OrtTypeInfo(ORT_TYPE_MAP, nullptr);
    return nullptr;
  }
  if (input->has_opaque_type()) {
    *out = new OrtTypeInfo(ORT_TYPE_OPAQUE, nullptr);
    return nullptr;
  }
  if (input->has_sparse_tensor_type()) {
    *out = new OrtTypeInfo(ORT_TYPE_SPARSETENSOR, nullptr);
    return nullptr;
  }
  return CreateONNXStatus(ORT_NOT_IMPLEMENTED, "not implemented");
}