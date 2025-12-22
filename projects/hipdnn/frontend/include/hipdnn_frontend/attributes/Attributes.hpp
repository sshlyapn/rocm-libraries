// Copyright © Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier:  MIT
#pragma once

#include "GraphAttributes.hpp"
#include "TensorAttributes.hpp"
#include <hipdnn_frontend/Error.hpp>
#include <hipdnn_frontend/Types.hpp>
#include <string>
#include <vector>

// Generates lvalue and rvalue setters for a data member.
// Returns a reference to the derived class.
// Parameters:
//   - DERIVED_CLASS: CRTP derived class type
//   - MEMBER_NAME:   Data member identifier
//   - MEMBER_TYPE:   Data member type
#define ATTRS_DEFAULT_MEMBER_SETTER(DERIVED_CLASS, MEMBER_NAME, MEMBER_TYPE)  \
    /* NOLINTNEXTLINE(readability-identifier-naming) */                       \
    DERIVED_CLASS& set_##MEMBER_NAME(const MEMBER_TYPE& value)                \
    {                                                                         \
        this->MEMBER_NAME = value;                                            \
        return static_cast<DERIVED_CLASS&>(*this);                            \
    }                                                                         \
    /* NOLINTNEXTLINE(readability-identifier-naming) */                       \
    DERIVED_CLASS& set_##MEMBER_NAME(MEMBER_TYPE&& value)                     \
    {                                                                         \
        this->MEMBER_NAME = std::move(value);                                 \
        return static_cast<DERIVED_CLASS&>(*this);                            \
    }

// Generates a const reference getter for a data member.
// Parameters:
//   - DERIVED_CLASS: CRTP derived class type (not used here, kept for symmetry)
//   - MEMBER_NAME:   Data member identifier
//   - MEMBER_TYPE:   Data member type
#define ATTRS_DEFAULT_MEMBER_GETTER(DERIVED_CLASS, MEMBER_NAME, MEMBER_TYPE)  \
    /* NOLINTNEXTLINE(readability-identifier-naming) */                       \
    const MEMBER_TYPE& get_##MEMBER_NAME() const                              \
    {                                                                         \
        static_assert(                                                        \
            std::is_same_v<MEMBER_TYPE, decltype(this->MEMBER_NAME)>,         \
            "Incorrect member type");                                         \
        return this->MEMBER_NAME;                                             \
    }

// Convenience macro to declare both getter and setters for a data member.
// See the two macros above for parameter descriptions.
#define ATTRS_DEFAULT_MEMBER_ACCESSOR(DERIVED_CLASS, MEMBER_NAME, MEMBER_TYPE) \
    ATTRS_DEFAULT_MEMBER_GETTER(DERIVED_CLASS, MEMBER_NAME, MEMBER_TYPE)       \
    ATTRS_DEFAULT_MEMBER_SETTER(DERIVED_CLASS, MEMBER_NAME, MEMBER_TYPE)

// Generates accessors for a specific named input or output tensor.
// Parameters:
//   - DERIVED_CLASS:    CRTP derived class type
//   - INPUT_OR_OUTPUT:  Either 'Input' or 'Output'
//   - TENSOR_NAME:      Accessor function base name (e.g., "x" -> get_x/set_x)
//   - TENSOR_NAME_TYPE: Enum identifying the tensor in the map
#define ATTRS_DETAIL_TENSOR_ACCESSOR_IMPL(DERIVED_CLASS, INPUT_OR_OUTPUT, TENSOR_NAME, TENSOR_NAME_TYPE) \
    /* NOLINTNEXTLINE(readability-identifier-naming) */                                                  \
    std::shared_ptr<TensorAttributes> get_##TENSOR_NAME() const                                          \
    {                                                                                                    \
        return this->get##INPUT_OR_OUTPUT(TENSOR_NAME_TYPE);                                             \
    }                                                                                                    \
    /* NOLINTNEXTLINE(readability-identifier-naming) */                                                  \
    DERIVED_CLASS& set_##TENSOR_NAME(const std::shared_ptr<TensorAttributes>& value)                     \
    {                                                                                                    \
        return this->set##INPUT_OR_OUTPUT(TENSOR_NAME_TYPE, value);                                      \
    }                                                                                                    \
    /* NOLINTNEXTLINE(readability-identifier-naming) */                                                  \
    DERIVED_CLASS& set_##TENSOR_NAME(std::shared_ptr<TensorAttributes>&& value)                          \
    {                                                                                                    \
        return this->set##INPUT_OR_OUTPUT(TENSOR_NAME_TYPE, std::move(value));                           \
    }

// Specialization helper for input tensor accessor generation.
#define ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(DERIVED_CLASS, TENSOR_NAME, TENSOR_NAME_TYPE) \
    ATTRS_DETAIL_TENSOR_ACCESSOR_IMPL(DERIVED_CLASS, Input, TENSOR_NAME, TENSOR_NAME_TYPE)

// Specialization helper for output tensor accessor generation.
#define ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(DERIVED_CLASS, TENSOR_NAME, TENSOR_NAME_TYPE) \
    ATTRS_DETAIL_TENSOR_ACCESSOR_IMPL(DERIVED_CLASS, Output, TENSOR_NAME, TENSOR_NAME_TYPE)

namespace hipdnn_frontend::graph
{
// Any class extending Attributes must have an inputs & outputs map.
// The map needs to have TensorAttributes as the value.
// Attributes uses these maps to set the tensor data types.
template <typename DerivedT>
class Attributes
{
private:
    DerivedT& self()
    {
        return static_cast<DerivedT&>(*this);
    }
    const DerivedT& self() const
    {
        return static_cast<const DerivedT&>(*this);
    }

public:
    std::string name;
    DataType compute_data_type = DataType::NOT_SET; // NOLINT(readability-identifier-naming)

    DerivedT& set_name(const std::string& nameValue) // NOLINT(readability-identifier-naming)
    {
        name = nameValue;
        return self();
    }

    const std::string& get_name() const // NOLINT(readability-identifier-naming)
    {
        return name;
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    DerivedT& set_compute_data_type(DataType value)
    {
        compute_data_type = value;
        return self();
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    DataType get_compute_data_type() const
    {
        return compute_data_type;
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    Error fill_from_context(const GraphAttributes& graphAttributes)
    {
        for(auto& [_, tensor] : self().inputs)
        {
            if(tensor)
            {
                tensor->fill_from_context(graphAttributes);
            }
        }

        for(auto& [_, tensor] : self().outputs)
        {
            if(tensor)
            {
                tensor->fill_from_context(graphAttributes);
            }
        }

        if(get_compute_data_type() == DataType::NOT_SET)
        {
            set_compute_data_type(graphAttributes.get_compute_data_type());
        }

        return {};
    }

protected:
    template <typename InputNameT>
    std::shared_ptr<TensorAttributes> getInput(InputNameT inputName) const
    {
        auto it = self().inputs.find(inputName);
        if(it != self().inputs.end())
        {
            return it->second;
        }
        return nullptr;
    }

    template <typename OutputNameT>
    std::shared_ptr<TensorAttributes> getOutput(OutputNameT outputName) const
    {
        auto it = self().outputs.find(outputName);
        if(it != self().outputs.end())
        {
            return it->second;
        }
        return nullptr;
    }

    template <typename InputNameT>
    DerivedT& setInput(InputNameT inputName,
                       const std::shared_ptr<TensorAttributes>& value)
    {
        self().inputs[inputName] = value;
        return self();
    }

    template <typename InputNameT>
    DerivedT& setInput(InputNameT inputName,
                       std::shared_ptr<TensorAttributes>&& value)
    {
        self().inputs[inputName] = std::move(value);
        return self();
    }

    template <typename OutputNameT>
    DerivedT& setOutput(OutputNameT outputName,
                        const std::shared_ptr<TensorAttributes>& value)
    {
        self().outputs[outputName] = value;
        return self();
    }

    template <typename OutputNameT>
    DerivedT& setOutput(OutputNameT outputName,
                        std::shared_ptr<TensorAttributes>&& value)
    {
        self().outputs[outputName] = std::move(value);
        return self();
    }
};
} // namespace hipdnn_frontend::graph
