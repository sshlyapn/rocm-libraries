// Copyright © Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier:  MIT
#pragma once

#include "Attributes.hpp"
#include "TensorAttributes.hpp"
#include <hipdnn_sdk/data_objects/batchnorm_inference_attributes_generated.h>
#include <memory>
#include <unordered_map>

namespace hipdnn_frontend::graph
{
class BatchnormInferenceAttributes : public Attributes<BatchnormInferenceAttributes>
{
public:
    enum class InputNames
    {
        X = 0,
        MEAN = 1,
        INV_VARIANCE = 2,
        SCALE = 3,
        BIAS = 4
    };
    typedef InputNames input_names; // NOLINT(readability-identifier-naming)

    enum class OutputNames
    {
        Y = 0
    };
    typedef OutputNames output_names; // NOLINT(readability-identifier-naming)

    std::unordered_map<InputNames, std::shared_ptr<TensorAttributes>> inputs;
    std::unordered_map<OutputNames, std::shared_ptr<TensorAttributes>> outputs;

    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormInferenceAttributes, x, InputNames::X)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormInferenceAttributes, mean, InputNames::MEAN)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormInferenceAttributes, inv_variance, InputNames::INV_VARIANCE)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormInferenceAttributes, scale, InputNames::SCALE)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormInferenceAttributes, bias, InputNames::BIAS)

    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(BatchnormInferenceAttributes, y, OutputNames::Y)

    // (Accessors above)

    flatbuffers::Offset<hipdnn_sdk::data_objects::BatchnormInferenceAttributes>
        pack_attributes(flatbuffers::FlatBufferBuilder& builder) const // NOLINT
    {
        auto mean = get_mean();
        auto invVariance = get_inv_variance();

        return hipdnn_sdk::data_objects::CreateBatchnormInferenceAttributes(builder,
                                                                            get_x()->get_uid(),
                                                                            mean->get_uid(),
                                                                            invVariance->get_uid(),
                                                                            get_scale()->get_uid(),
                                                                            get_bias()->get_uid(),
                                                                            get_y()->get_uid());
    }
};
typedef BatchnormInferenceAttributes Batchnorm_inference_attributes;
} // namespace hipdnn_frontend::graph
