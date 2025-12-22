// Copyright © Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier:  MIT
#pragma once

#include "Attributes.hpp"
#include "TensorAttributes.hpp"
#include <hipdnn_frontend/Types.hpp>
#include <hipdnn_sdk/data_objects/convolution_wrw_attributes_generated.h>
#include <memory>
#include <unordered_map>
#include <vector>

namespace hipdnn_frontend::graph
{
class ConvWgradAttributes : public Attributes<ConvWgradAttributes>
{
public:
    enum class InputNames
    {
        DY = 0, // Gradient of output tensor
        X = 1 // Input tensor
    };
    typedef InputNames input_names; // NOLINT(readability-identifier-naming)

    enum class OutputNames
    {
        DW = 0 // Gradient of weights tensor
    };
    typedef OutputNames output_names; // NOLINT(readability-identifier-naming)

    std::unordered_map<InputNames, std::shared_ptr<TensorAttributes>> inputs;
    std::unordered_map<OutputNames, std::shared_ptr<TensorAttributes>> outputs;

    std::vector<int64_t> pre_padding; // NOLINT(readability-identifier-naming)
    std::vector<int64_t> post_padding; // NOLINT(readability-identifier-naming)
    std::vector<int64_t> stride;
    std::vector<int64_t> dilation;
    // NOLINTNEXTLINE(readability-identifier-naming)
    ConvolutionMode convolution_mode = ConvolutionMode::CROSS_CORRELATION;

    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(ConvWgradAttributes, x, InputNames::X)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(ConvWgradAttributes, dy, InputNames::DY)

    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(ConvWgradAttributes, dw, OutputNames::DW)

    ATTRS_DEFAULT_MEMBER_ACCESSOR(ConvWgradAttributes, pre_padding, std::vector<int64_t>)
    ATTRS_DEFAULT_MEMBER_ACCESSOR(ConvWgradAttributes, post_padding, std::vector<int64_t>)
    ATTRS_DEFAULT_MEMBER_ACCESSOR(ConvWgradAttributes, stride, std::vector<int64_t>)
    ATTRS_DEFAULT_MEMBER_ACCESSOR(ConvWgradAttributes, dilation, std::vector<int64_t>)
    ATTRS_DEFAULT_MEMBER_ACCESSOR(ConvWgradAttributes, convolution_mode, ConvolutionMode)

    // Setters for tensor
    // (Accessors above)
    // NOLINTNEXTLINE(readability-identifier-naming)
    ConvWgradAttributes& set_padding(std::vector<int64_t> padding)
    {
        set_pre_padding(padding);
        set_post_padding(std::move(padding));
        return *this;
    }

    flatbuffers::Offset<hipdnn_sdk::data_objects::ConvolutionWrwAttributes>
        pack_attributes(flatbuffers::FlatBufferBuilder& builder) const // NOLINT
    {
        return hipdnn_sdk::data_objects::CreateConvolutionWrwAttributesDirect(builder,
                                                                              get_x()->get_uid(),
                                                                              get_dy()->get_uid(),
                                                                              get_dw()->get_uid(),
                                                                              &pre_padding,
                                                                              &post_padding,
                                                                              &stride,
                                                                              &dilation,
                                                                              toSdkType(convolution_mode));
    }
};
typedef ConvWgradAttributes Conv_wgrad_attributes;
} // namespace hipdnn_frontend::graph
