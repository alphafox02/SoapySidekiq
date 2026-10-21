/**
 * @file RxBlockAligner.hpp
 * @brief Aligns interleaved receive blocks into bounded, planar channel groups.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace soapy_sidekiq
{

/** Owned sample planes sharing one RF sample-counter interval. */
struct AlignedRxBlock
{
    std::vector<std::vector<std::int16_t>> channels;
    std::uint64_t rf_timestamp{};
    /** Original system timestamp of channel zero's source block. */
    std::uint64_t system_timestamp{};
    /** Exact sample offset from that source block, before time conversion. */
    std::uint64_t system_sample_offset{};
    bool discontinuity_before{};
};

/**
 * Single-producer alignment stage. The receive worker alone owns this object;
 * completed groups are handed to the synchronized sample queue. Channel indices
 * follow the caller's requested buffer order, not hardware handle order.
 *
 * Blocks must have a shared RF counter domain and sample rate. Forward gaps and
 * bounded-queue eviction are recoverable and mark the next aligned group. RF
 * counters that overlap or move backwards are rejected; an active counter reset
 * cannot safely be paired with samples from another counter epoch.
 */
class RxBlockAligner
{
  public:
    /** Both channel count and per-channel pending-block capacity must be positive. */
    RxBlockAligner(std::size_t channels, std::size_t pending_capacity);

    /**
     * Accept an owned hardware block for one selected channel.
     * @return True when its RF timestamp is not contiguous with the prior block.
     * @throws std::invalid_argument for invalid planes or overlapping counters.
     * @throws std::overflow_error when the RF interval cannot be represented.
     */
    bool push(std::size_t channel, std::vector<std::int16_t> samples, std::uint64_t rf_timestamp,
              std::uint64_t system_timestamp);

    /** Mark the next incoming block on each channel after an unlocalized overrun. */
    void markDiscontinuity();

    /** Return the next common RF interval, or nullopt until every channel is ready. */
    std::optional<AlignedRxBlock> pop();

  private:
    struct Block
    {
        std::vector<std::int16_t> samples;
        std::uint64_t rf_timestamp{}, system_timestamp{};
        std::size_t offset{};
        bool gap{};
    };
    struct Channel
    {
        std::deque<Block> blocks;
        std::uint64_t last_end{};
        bool have_last{}, next_gap{};
    };
    std::vector<Channel> channels_;
    std::size_t pending_capacity_;
    std::uint64_t last_group_end_{};
    bool have_group_{};
};

} // namespace soapy_sidekiq
