// Copyright © Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier:  MIT
#pragma once

#include "Attributes.hpp"
#include "TensorAttributes.hpp"
#include <hipdnn_sdk/data_objects/batchnorm_backward_attributes_generated.h>
#include <memory>
#include <unordered_map>

namespace hipdnn_frontend::graph
{
class BatchnormBackwardAttributes : public Attributes<BatchnormBackwardAttributes>
{
public:
    enum class InputNames
    {
        DY = 0,
        X = 1,
        SCALE = 2,
        MEAN = 3,
        INV_VARIANCE = 4
    };
    typedef InputNames input_names; // NOLINT(readability-identifier-naming)

    enum class OutputNames
    {
        DX = 0,
        DSCALE = 1,
        DBIAS = 2
    };
    typedef OutputNames output_names; // NOLINT(readability-identifier-naming)

    std::unordered_map<InputNames, std::shared_ptr<TensorAttributes>> inputs;
    std::unordered_map<OutputNames, std::shared_ptr<TensorAttributes>> outputs;
    // NOLINTNEXTLINE(readability-identifier-naming)
    std::vector<std::shared_ptr<TensorAttributes>> peer_stats;

    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormBackwardAttributes, dy, InputNames::DY)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormBackwardAttributes, x, InputNames::X)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormBackwardAttributes, scale, InputNames::SCALE)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormBackwardAttributes, mean, InputNames::MEAN)
    ATTRS_DEFAULT_INPUT_TENSOR_ACCESSOR(BatchnormBackwardAttributes, inv_variance, InputNames::INV_VARIANCE)

    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(BatchnormBackwardAttributes, dx, OutputNames::DX)
    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(BatchnormBackwardAttributes, dscale, OutputNames::DSCALE)
    ATTRS_DEFAULT_OUTPUT_TENSOR_ACCESSOR(BatchnormBackwardAttributes, dbias, OutputNames::DBIAS)

    ATTRS_DEFAULT_MEMBER_ACCESSOR(BatchnormBackwardAttributes, peer_stats, std::vector<std::shared_ptr<TensorAttributes>>)

    // (Accessors above)
    // NOLINTNEXTLINE(readability-identifier-naming)

    BatchnormBackwardAttributes&
        set_saved_mean_and_inv_variance(const std::shared_ptr<TensorAttributes>& mean, // NOLINT
                                        const std::shared_ptr<TensorAttributes>& invVariance)
    {
        return set_mean(mean).set_inv_variance(invVariance);
    }
    BatchnormBackwardAttributes&
        set_saved_mean_and_inv_variance(std::shared_ptr<TensorAttributes>&& mean, // NOLINT
                                        std::shared_ptr<TensorAttributes>&& invVariance)
    {
        return set_mean(std::move(mean)).set_inv_variance(std::move(invVariance));
    }

    flatbuffers::Offset<hipdnn_sdk::data_objects::BatchnormBackwardAttributes>
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

        auto mean = get_mean();
        auto invVariance = get_inv_variance();

        return hipdnn_sdk::data_objects::CreateBatchnormBackwardAttributesDirect(
            builder,
            get_dy()->get_uid(),
            get_x()->get_uid(),
            mean ? flatbuffers::Optional<int64_t>(mean->get_uid()) : flatbuffers::nullopt,
            invVariance ? flatbuffers::Optional<int64_t>(invVariance->get_uid())
                        : flatbuffers::nullopt,
            get_scale()->get_uid(),
            &peerStatsVector,
            get_dx()->get_uid(),
            get_dscale()->get_uid(),
            get_dbias()->get_uid());
    }
};
typedef BatchnormBackwardAttributes Batchnorm_backward_attributes;
} // namespace hipdnn_frontend::graph
