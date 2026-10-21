/**
 * @file RxBlockAligner.cpp
 * @brief Implements RF-counter alignment before synchronized multi-channel reads.
 */

#include <SoapySidekiq/RxBlockAligner.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace soapy_sidekiq
{

RxBlockAligner::RxBlockAligner(const std::size_t channels, const std::size_t pending_capacity)
    : channels_(channels), pending_capacity_(pending_capacity)
{
    if (channels == 0 || pending_capacity == 0)
        throw std::invalid_argument("RX alignment requires positive channel count and capacity");
}

bool RxBlockAligner::push(const std::size_t channel, std::vector<std::int16_t> samples,
                          const std::uint64_t rf_timestamp, const std::uint64_t system_timestamp)
{
    if (channel >= channels_.size())
        throw std::out_of_range("RX alignment channel is unavailable");
    if (samples.empty() || samples.size() % 2 != 0)
        throw std::invalid_argument("RX alignment requires complete nonempty IQ samples");
    const auto count = samples.size() / 2;
    if (count > std::numeric_limits<std::uint64_t>::max() - rf_timestamp)
        throw std::overflow_error("RX RF sample interval exceeds uint64_t");
    auto &state = channels_[channel];
    if (state.have_last && rf_timestamp < state.last_end)
        throw std::invalid_argument("RX RF counters overlap or moved backwards");
    const bool discontinuity = state.have_last && rf_timestamp != state.last_end;
    state.blocks.push_back(
        {std::move(samples), rf_timestamp, system_timestamp, 0, state.next_gap || discontinuity});
    state.next_gap = false;
    state.last_end = rf_timestamp + count;
    state.have_last = true;
    if (state.blocks.size() > pending_capacity_)
    {
        state.blocks.pop_front();
        state.blocks.front().gap = true;
    }
    return discontinuity;
}

void RxBlockAligner::markDiscontinuity()
{
    for (auto &channel : channels_)
        channel.next_gap = true;
}

std::optional<AlignedRxBlock> RxBlockAligner::pop()
{
    std::uint64_t target = 0;
    for (;;)
    {
        for (const auto &channel : channels_)
        {
            if (channel.blocks.empty())
                return std::nullopt;
            const auto &front = channel.blocks.front();
            target = std::max(target, front.rf_timestamp + front.offset);
        }
        bool aligned = true;
        for (auto &channel : channels_)
        {
            while (channel.blocks.front().rf_timestamp +
                       channel.blocks.front().samples.size() / 2 <=
                   target)
            {
                channel.blocks.pop_front();
                if (channel.blocks.empty())
                {
                    channel.next_gap = true;
                    return std::nullopt;
                }
                channel.blocks.front().gap = true;
            }
            auto &front = channel.blocks.front();
            const auto start = front.rf_timestamp + front.offset;
            if (start > target)
            {
                aligned = false;
                continue;
            }
            if (start < target)
            {
                front.offset += static_cast<std::size_t>(target - start);
                front.gap = true;
            }
        }
        if (aligned)
            break;
    }

    std::size_t count = std::numeric_limits<std::size_t>::max();
    AlignedRxBlock result;
    result.channels.resize(channels_.size());
    result.rf_timestamp = target;
    result.system_timestamp = channels_.front().blocks.front().system_timestamp;
    result.system_sample_offset = channels_.front().blocks.front().offset;
    result.discontinuity_before = have_group_ && target != last_group_end_;
    for (const auto &channel : channels_)
    {
        const auto &front = channel.blocks.front();
        count = std::min(count, front.samples.size() / 2 - front.offset);
        result.discontinuity_before = result.discontinuity_before || front.gap;
    }
    // Allocate/copy cropped planes before moving any complete plane. Allocation
    // failure then leaves all source blocks intact for a caller's error path.
    for (std::size_t channel = 0; channel < channels_.size(); ++channel)
    {
        const auto &front = channels_[channel].blocks.front();
        if (front.offset != 0 || count != front.samples.size() / 2)
            result.channels[channel].assign(front.samples.data() + front.offset * 2,
                                            front.samples.data() + (front.offset + count) * 2);
    }
    for (std::size_t channel = 0; channel < channels_.size(); ++channel)
    {
        auto &pending = channels_[channel].blocks;
        auto &front = pending.front();
        const auto total = front.samples.size() / 2;
        if (front.offset == 0 && count == total)
            result.channels[channel] = std::move(front.samples);
        front.offset += count;
        front.gap = false;
        if (front.offset == total)
            pending.pop_front();
    }
    have_group_ = true;
    last_group_end_ = target + count;
    return result;
}

} // namespace soapy_sidekiq
