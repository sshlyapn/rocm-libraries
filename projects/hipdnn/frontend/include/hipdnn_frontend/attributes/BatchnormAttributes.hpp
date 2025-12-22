// Copyright © Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier:  MIT
#pragma once

#include "Attributes.hpp"
#include "TensorAttributes.hpp"
#include <hipdnn_sdk/data_objects/batchnorm_attributes_generated.h>
#include <memory>
#include <unordered_map>
#include <vector>

namespace hipdnn_frontend::graph
{
class BatchnormAttributes : public Attributes<BatchnormAttributes>
{
public:
    enum class InputNames
    {
        X = 0,
        SCALE = 1,
        BIAS = 2,
        PREV_RUNNING_MEAN = 3,
        PREV_RUNNING_VARIANCE = 4,
        MOMENTUM = 5,
        EPSILON = 6
    };
    typedef InputNames input_names; // NOLINT(readability-identifier-naming)

    enum class OutputNames
    {
        Y = 0,
        MEAN = 1,
        INV_VARIANCE = 2,
        NEXT_RUNNING_MEAN = 3,
        NEXT_RUNNING_VARIANCE = 4
    };
    typedef OutputNames output_names; // NOLINT(readability-identifier-naming)

    std::unordered_map<InputNames, std::shared_ptr<TensorAttributes>> inputs;
    std::unordered_map<OutputNames, std::shared_ptr<TensorAttributes>> outputs;
    // NOLINTNEXTLINE(readability-identifier-naming)
    std::vector<std::shared_ptr<TensorAttributes>> peer_stats;

    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormAttributes, x, InputNames::X)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormAttributes, scale, InputNames::SCALE)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormAttributes, bias, InputNames::BIAS)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormAttributes, prev_running_mean, InputNames::PREV_RUNNING_MEAN)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormAttributes, prev_running_variance, InputNames::PREV_RUNNING_VARIANCE)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormAttributes, momentum, InputNames::MOMENTUM)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormAttributes, epsilon, InputNames::EPSILON)

    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(BatchnormAttributes, y, OutputNames::Y)
    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(BatchnormAttributes, mean, OutputNames::MEAN)
    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(BatchnormAttributes, inv_variance, OutputNames::INV_VARIANCE)
    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(BatchnormAttributes, next_running_mean, OutputNames::NEXT_RUNNING_MEAN)
    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(BatchnormAttributes, next_running_variance, OutputNames::NEXT_RUNNING_VARIANCE)

    ATTRS_DEFAULT_MEMBER_ACCESSOR(BatchnormAttributes, peer_stats, std::vector<std::shared_ptr<TensorAttributes>>)

    // NOLINTBEGIN(readability-identifier-naming)
    BatchnormAttributes&
        set_previous_running_stats(const std::shared_ptr<TensorAttributes>& mean,
                                   const std::shared_ptr<TensorAttributes>& variance,
                                   const std::shared_ptr<TensorAttributes>& momentum)
    // NOLINTEND(readability-identifier-naming)
    {
        return set_prev_running_mean(mean).set_prev_running_variance(variance).set_momentum(
            momentum);
    }
    // NOLINTNEXTLINE(readability-identifier-naming)
    BatchnormAttributes& set_previous_running_stats(std::shared_ptr<TensorAttributes>&& mean,
                                                    std::shared_ptr<TensorAttributes>&& variance,
                                                    std::shared_ptr<TensorAttributes>&& momentum)
    {
        return set_prev_running_mean(std::move(mean))
            .set_prev_running_variance(std::move(variance))
            .set_momentum(std::move(momentum));
    }

    flatbuffers::Offset<hipdnn_sdk::data_objects::BatchnormAttributes>
        pack_attributes(flatbuffers::FlatBufferBuilder& builder) const // NOLINT
    {
        auto peerStatsVector = std::vector<int64_t>{};
        for(const auto& peerStat : peer_stats)
        {
            if(peerStat)
            {
                peerStatsVector.emplace_back(peerStat->get_uid());
            }
        }

        auto prevRunningMean = get_prev_running_mean();
        auto prevRunningVariance = get_prev_running_variance();
        auto momentum = get_momentum();
        auto mean = get_mean();
        auto invVariance = get_inv_variance();
        auto nextRunningMean = get_next_running_mean();
        auto nextRunningVariance = get_next_running_variance();

        return hipdnn_sdk::data_objects::CreateBatchnormAttributesDirect(
            builder,
            get_x()->get_uid(),
            get_scale()->get_uid(),
            get_bias()->get_uid(),
            get_epsilon()->get_uid(),
            &peerStatsVector,
            prevRunningMean ? flatbuffers::Optional<int64_t>(prevRunningMean->get_uid())
                            : flatbuffers::nullopt,
            prevRunningVariance ? flatbuffers::Optional<int64_t>(prevRunningVariance->get_uid())
                                : flatbuffers::nullopt,
            momentum ? flatbuffers::Optional<int64_t>(momentum->get_uid()) : flatbuffers::nullopt,
            get_y()->get_uid(),
            mean ? flatbuffers::Optional<int64_t>(mean->get_uid()) : flatbuffers::nullopt,
            invVariance ? flatbuffers::Optional<int64_t>(invVariance->get_uid())
                        : flatbuffers::nullopt,
            nextRunningMean ? flatbuffers::Optional<int64_t>(nextRunningMean->get_uid())
                            : flatbuffers::nullopt,
            nextRunningVariance ? flatbuffers::Optional<int64_t>(nextRunningVariance->get_uid())
                                : flatbuffers::nullopt);
    }
};

typedef BatchnormAttributes Batchnorm_attributes;
} // namespace hipdnn_frontend::graph
