#ifndef CORE_PROVIDERS_CPU_ACTIVATION_RELU_H
#define CORE_PROVIDERS_CPU_ACTIVATION_RELU_H

#include "core/framework/op_kernel.h"
#include "core/common/common.h"
#include "core/util/math_cpuonly.h"

namespace Lotus {

    template<typename T>
    class ReLU final : public OpKernel {

    public:
        ReLU(OpKernelInfo* info) : OpKernel(info)
        {
        }

        void compute(OpKernelContext* context) override;
    };

}

#endif // !CORE_PROVIDERS_CPU_ACTIVATION_RELU_H