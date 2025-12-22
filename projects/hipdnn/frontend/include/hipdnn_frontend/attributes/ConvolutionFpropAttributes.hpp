// Copyright © Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier:  MIT
#pragma once

#include "Attributes.hpp"
#include "TensorAttributes.hpp"
#include <hipdnn_frontend/Types.hpp>
#include <hipdnn_sdk/data_objects/convolution_fwd_attributes_generated.h>
#include <memory>
#include <unordered_map>
#include <vector>

namespace hipdnn_frontend::graph
{
class ConvFpropAttributes : public Attributes<ConvFpropAttributes>
{
public:
    enum class InputNames
    {
        X = 0, // Input tensor
        W = 1 // Weights/filter tensor
    };
    typedef InputNames input_names; // NOLINT(readability-identifier-naming)

    enum class OutputNames
    {
        Y = 0 // Output tensor
    };
    typedef OutputNames output_names; // NOLINT(readability-identifier-naming)

    std::unordered_map<InputNames, std::shared_ptr<TensorAttributes>> inputs;
    std::unordered_map<OutputNames, std::shared_ptr<TensorAttributes>> outputs;

    // Convolution parameters
    std::vector<int64_t> pre_padding; // NOLINT(readability-identifier-naming)
    std::vector<int64_t> post_padding; // NOLINT(readability-identifier-naming)
    std::vector<int64_t> stride;
    std::vector<int64_t> dilation;
    // NOLINTNEXTLINE(readability-identifier-naming)
    ConvolutionMode convolution_mode = ConvolutionMode::CROSS_CORRELATION;

    // Getters/Setters for tensors
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(ConvFpropAttributes, x, InputNames::X)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(ConvFpropAttributes, w, InputNames::W)

    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(ConvFpropAttributes, y, OutputNames::Y)

    ATTRS_DEFAULT_MEMBER_ACCESSOR(ConvFpropAttributes, pre_padding, std::vector<int64_t>)
    ATTRS_DEFAULT_MEMBER_ACCESSOR(ConvFpropAttributes, post_padding, std::vector<int64_t>)
    ATTRS_DEFAULT_MEMBER_ACCESSOR(ConvFpropAttributes, stride, std::vector<int64_t>)
    ATTRS_DEFAULT_MEMBER_ACCESSOR(ConvFpropAttributes, dilation, std::vector<int64_t>)
    ATTRS_DEFAULT_MEMBER_ACCESSOR(ConvFpropAttributes, convolution_mode, ConvolutionMode)

    // NOLINTNEXTLINE(readability-identifier-naming)
    ConvFpropAttributes& set_padding(std::vector<int64_t> padding)
    {
        pre_padding = padding;
        post_padding = std::move(padding);
        return *this;
    }

    flatbuffers::Offset<hipdnn_sdk::data_objects::ConvolutionFwdAttributes>
        pack_attributes(flatbuffers::FlatBufferBuilder& builder) const // NOLINT
    {
        return hipdnn_sdk::data_objects::CreateConvolutionFwdAttributesDirect(builder,
                                                                              get_x()->get_uid(),
                                                                              get_w()->get_uid(),
                                                                              get_y()->get_uid(),
                                                                              &pre_padding,
                                                                              &post_padding,
                                                                              &stride,
                                                                              &dilation,
                                                                              toSdkType(convolution_mode));
    }
};
typedef ConvFpropAttributes Conv_fprop_attributes;
} // namespace hipdnn_frontend::graph
