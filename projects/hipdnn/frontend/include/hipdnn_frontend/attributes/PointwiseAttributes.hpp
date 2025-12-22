// Copyright © Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier:  MIT
#pragma once

#include "Attributes.hpp"
#include "TensorAttributes.hpp"
#include <hipdnn_frontend/Types.hpp>
#include <hipdnn_sdk/data_objects/tensor_attributes_generated.h>
#include <memory>
#include <optional>
#include <unordered_map>

namespace hipdnn_frontend::graph
{
class PointwiseAttributes : public Attributes<PointwiseAttributes>
{
public:
    enum class InputNames
    {   
        IN_0 = 0,
        IN_1 = 1,
        IN_2 = 2,
    };
    typedef InputNames input_names; // NOLINT(readability-identifier-naming)

    enum class OutputNames
    {
        OUT_0 = 0,
    };
    typedef OutputNames output_names; // NOLINT(readability-identifier-naming)

    std::unordered_map<InputNames, std::shared_ptr<TensorAttributes>> inputs;
    std::unordered_map<OutputNames, std::shared_ptr<TensorAttributes>> outputs;

    // NOLINTBEGIN(readability-identifier-naming)
    PointwiseMode mode = PointwiseMode::NOT_SET;
    std::optional<float> relu_lower_clip = std::nullopt;
    std::optional<float> relu_upper_clip = std::nullopt;
    std::optional<float> relu_lower_clip_slope = std::nullopt;
    std::optional<int64_t> axis = std::nullopt;
    std::optional<float> swish_beta = std::nullopt;
    std::optional<float> elu_alpha = std::nullopt;
    std::optional<float> softplus_beta = std::nullopt;
    // NOLINTEND(readability-identifier-naming)

    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(PointwiseAttributes, input_0, InputNames::IN_0)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(PointwiseAttributes, input_1, InputNames::IN_1)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(PointwiseAttributes, input_2, InputNames::IN_2)
    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(PointwiseAttributes, output_0, OutputNames::OUT_0)

    ATTRS_DEFAULT_MEMBER_ACCESSOR(PointwiseAttributes, mode, PointwiseMode)

    ATTRS_DEFAULT_MEMBER_GETTER(PointwiseAttributes, relu_lower_clip, std::optional<float>)
    ATTRS_DEFAULT_MEMBER_SETTER(PointwiseAttributes, relu_lower_clip, float)

    ATTRS_DEFAULT_MEMBER_GETTER(PointwiseAttributes, relu_upper_clip, std::optional<float>)
    ATTRS_DEFAULT_MEMBER_SETTER(PointwiseAttributes, relu_upper_clip, float)

    ATTRS_DEFAULT_MEMBER_GETTER(PointwiseAttributes, relu_lower_clip_slope, std::optional<float>)
    ATTRS_DEFAULT_MEMBER_SETTER(PointwiseAttributes, relu_lower_clip_slope, float)

    ATTRS_DEFAULT_MEMBER_GETTER(PointwiseAttributes, axis, std::optional<int64_t>)
    ATTRS_DEFAULT_MEMBER_SETTER(PointwiseAttributes, axis, int64_t)

    ATTRS_DEFAULT_MEMBER_GETTER(PointwiseAttributes, swish_beta, std::optional<float>)
    ATTRS_DEFAULT_MEMBER_SETTER(PointwiseAttributes, swish_beta, float)

    ATTRS_DEFAULT_MEMBER_GETTER(PointwiseAttributes, elu_alpha, std::optional<float>)
    ATTRS_DEFAULT_MEMBER_SETTER(PointwiseAttributes, elu_alpha, float)

    ATTRS_DEFAULT_MEMBER_GETTER(PointwiseAttributes, softplus_beta, std::optional<float>)
    ATTRS_DEFAULT_MEMBER_SETTER(PointwiseAttributes, softplus_beta, float)

    flatbuffers::Offset<hipdnn_sdk::data_objects::PointwiseAttributes>
        pack_attributes(flatbuffers::FlatBufferBuilder& builder) const // NOLINT
    {
        auto in0 = get_input_0();
        auto in1 = get_input_1();
        auto in2 = get_input_2();
        auto ot0 = get_output_0();

        return hipdnn_sdk::data_objects::CreatePointwiseAttributes(
            builder,
            toSdkType(mode),
            relu_lower_clip,
            relu_upper_clip,
            relu_lower_clip_slope,
            axis,
            in0->get_uid(),
            in1 ? flatbuffers::Optional<int64_t>(in1->get_uid()) : flatbuffers::nullopt,
            in2 ? flatbuffers::Optional<int64_t>(in2->get_uid()) : flatbuffers::nullopt,
            ot0->get_uid(),
            swish_beta,
            elu_alpha,
            softplus_beta);
    }
};
typedef PointwiseAttributes Pointwise_attributes;
} // namespace hipdnn_frontend::graph
