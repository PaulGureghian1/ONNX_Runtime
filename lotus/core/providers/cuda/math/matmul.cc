#include "matmul.h"
#include "core/providers/cpu/math/matmul_helper.h"

namespace Lotus {
namespace Cuda {

ONNX_OPERATOR_KERNEL_EX(
    MatMul,
    kOnnxDomain,
    1,
    kCudaExecutionProvider,
    KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
    MatMul<float>);

template <>
Status MatMul<float>::ComputeInternal(OpKernelContext* ctx) const {
  const Tensor* left_X = ctx->Input<Tensor>(0);
  const Tensor* right_X = ctx->Input<Tensor>(1);

  MatMulComputeHelper helper;
  LOTUS_RETURN_IF_ERROR(helper.Compute(left_X->Shape(), right_X->Shape()));

  Tensor* Y = ctx->Output(0, helper.OutputShape());
  LOTUS_RETURN_IF_NOT(Y->Location().name == CUDA, "Output should be allocated on CUDA");

  const float alpha = 1.0f;
  const float beta = 0.0f;

  if (helper.OutputOffsets().size() == 1) {
    CUBLAS_RETURN_IF_ERROR(cublasSgemm(
        Base::CublasHandle(),
        CUBLAS_OP_N,
        CUBLAS_OP_N,
        static_cast<int>(helper.N()),
        static_cast<int>(helper.M()),
        static_cast<int>(helper.K()),
        &alpha,
        right_X->Data<float>(),
        static_cast<int>(helper.N()),
        left_X->Data<float>(),
        static_cast<int>(helper.K()),
        &beta,
        Y->MutableData<float>(),
        static_cast<int>(helper.N())));
    return Status::OK();
  }

  CudaAsyncBuffer<const float*> left_arrays(this, helper.LeftOffsets().size());
  CudaAsyncBuffer<const float*> right_arrays(this, helper.RightOffsets().size());
  CudaAsyncBuffer<float*> output_arrays(this, helper.OutputOffsets().size());
  MatMulComputeHelper::OffsetToArrays(left_X->Data<float>(), helper.LeftOffsets(), left_arrays.CpuSpan());
  MatMulComputeHelper::OffsetToArrays(right_X->Data<float>(), helper.RightOffsets(), right_arrays.CpuSpan());
  MatMulComputeHelper::OffsetToArrays(Y->MutableData<float>(), helper.OutputOffsets(), output_arrays.CpuSpan());
  LOTUS_RETURN_IF_ERROR(left_arrays.CopyToGpu());
  LOTUS_RETURN_IF_ERROR(right_arrays.CopyToGpu());
  LOTUS_RETURN_IF_ERROR(output_arrays.CopyToGpu());

  // note that Lotus MLValue is row major, while cublas is column major,
  // so swap left/right operands
  CUBLAS_RETURN_IF_ERROR(cublasSgemmBatched(
      Base::CublasHandle(),
      CUBLAS_OP_N,
      CUBLAS_OP_N,
      static_cast<int>(helper.N()),
      static_cast<int>(helper.M()),
      static_cast<int>(helper.K()),
      &alpha,
      right_arrays.GpuPtr(),
      static_cast<int>(helper.N()),
      left_arrays.GpuPtr(),
      static_cast<int>(helper.K()),
      &beta,
      output_arrays.GpuPtr(),
      static_cast<int>(helper.N()),
      static_cast<int>(helper.OutputOffsets().size())));

  return Status::OK();
}

}  // namespace Cuda
}  // namespace Lotus
