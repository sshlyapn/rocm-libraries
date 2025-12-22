// Copyright © Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier:  MIT
#pragma once

#include "Attributes.hpp"
#include "TensorAttributes.hpp"
#include <hipdnn_sdk/data_objects/matmul_attributes_generated.h>
#include <memory>
#include <unordered_map>
#include <vector>


namespace hipdnn_frontend::graph
{
class MatmulAttributes : public Attributes<MatmulAttributes>
{
public:
    enum class InputNames
    {
        A = 0, // Input A tensor
        B = 1 // Input b tensor
    };
    typedef InputNames input_names; // NOLINT(readability-identifier-naming)

    enum class OutputNames
    {
        C = 0 // Output tensor
    };
    typedef OutputNames output_names; // NOLINT(readability-identifier-naming)

    std::unordered_map<InputNames, std::shared_ptr<TensorAttributes>> inputs;
    std::unordered_map<OutputNames, std::shared_ptr<TensorAttributes>> outputs;

    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(MatmulAttributes, a, InputNames::A)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(MatmulAttributes, b, InputNames::B)

    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(MatmulAttributes, c, OutputNames::C)

    flatbuffers::Offset<hipdnn_sdk::data_objects::MatmulAttributes>
        pack_attributes(flatbuffers::FlatBufferBuilder& builder) const // NOLINT
    {
        return hipdnn_sdk::data_objects::CreateMatmulAttributes(
            builder, get_a()->get_uid(), get_b()->get_uid(), get_c()->get_uid());
    }
};

typedef MatmulAttributes Batchnorm_attributes;
} // namespace hipdnn_frontend::graph
