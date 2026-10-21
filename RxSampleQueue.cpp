/**
 * @file RxSampleQueue.cpp
 * @brief Implements synchronized RX block ownership, waiting, and timestamps.
 */

#include <SoapySidekiq/RxSampleQueue.hpp>
#include <SoapySidekiq/TimestampConversion.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace soapy_sidekiq
{
RxSampleQueue::RxSampleQueue(const std::size_t capacity_blocks, const std::size_t channel_count)
    : capacity_blocks_(capacity_blocks), channel_count_(channel_count)
{
    if (capacity_blocks_ == 0 || channel_count_ == 0)
    {
        throw std::invalid_argument("RX queue capacity and channel count must be positive");
    }
}

void RxSampleQueue::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // A reactivated stream must never expose samples or errors from its prior run.
    blocks_.clear();
    running_ = true;
    failed_ = false;
    next_block_discontinuous_ = false;
    dropped_blocks_ = 0;
}

void RxSampleQueue::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    // A reader may otherwise remain asleep until its entire timeout expires.
    data_available_.notify_all();
}

void RxSampleQueue::fail()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        failed_ = true;
    }
    // Failure is terminal for this session and must be visible to every reader.
    data_available_.notify_all();
}

void RxSampleQueue::reset()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        blocks_.clear();
        running_ = false;
        failed_ = false;
        next_block_discontinuous_ = false;
        dropped_blocks_ = 0;
    }
    data_available_.notify_all();
}

void RxSampleQueue::markDiscontinuity()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
        next_block_discontinuous_ = true;
}

RxPushStatus RxSampleQueue::push(const std::int16_t *samples, const std::size_t complex_samples,
                                 const std::int64_t time_ns, const TimestampRate sample_rate)
{
    if (complex_samples == 0 || complex_samples > std::numeric_limits<std::size_t>::max() / 2)
        throw std::invalid_argument("RX sample count must be positive");
    if (samples == nullptr) throw std::invalid_argument("RX samples cannot be null");
    return push(std::vector<std::int16_t>(samples, samples + complex_samples * 2),
                time_ns, sample_rate);
}

RxPushStatus RxSampleQueue::pushTicks(const std::int16_t *samples,
                                      const std::size_t complex_samples,
                                      const std::uint64_t timestamp,
                                      const TimestampRate timestamp_frequency,
                                      const TimestampRate sample_rate)
{
    if (complex_samples == 0 || complex_samples > std::numeric_limits<std::size_t>::max() / 2)
        throw std::invalid_argument("RX sample count must be positive");
    if (samples == nullptr) throw std::invalid_argument("RX samples cannot be null");
    return pushTicks(std::vector<std::int16_t>(samples, samples + complex_samples * 2),
                     timestamp, timestamp_frequency, sample_rate);
}

RxPushStatus RxSampleQueue::pushTicks(std::vector<std::int16_t> samples,
                                      const std::uint64_t timestamp,
                                      const TimestampRate timestamp_frequency,
                                      const TimestampRate sample_rate)
{
    const auto phase = timestamp_frequency.convert(timestamp);
    return pushWithPhase(std::move(samples), phase.nanoseconds, sample_rate, phase.remainder,
                         phase.denominator);
}

RxPushStatus RxSampleQueue::push(std::vector<std::int16_t> samples, const std::int64_t time_ns,
                                 const TimestampRate sample_rate)
{
    return pushWithPhase(std::move(samples), time_ns, sample_rate, 0, 1);
}

RxPushStatus RxSampleQueue::pushWithPhase(std::vector<std::int16_t> samples,
                                          const std::int64_t time_ns,
                                          const TimestampRate sample_rate,
                                          const std::uint64_t remainder,
                                          const std::uint64_t denominator)
{
    std::vector<std::vector<std::int16_t>> planes;
    planes.push_back(std::move(samples));
    return pushChannelsWithPhase(std::move(planes), time_ns, sample_rate, remainder, denominator,
                                 0);
}

RxPushStatus RxSampleQueue::pushChannelsTicks(std::vector<std::vector<std::int16_t>> samples,
                                              const std::uint64_t timestamp,
                                              const TimestampRate timestamp_frequency,
                                              const TimestampRate sample_rate,
                                              const std::uint64_t timestamp_sample_offset)
{
    const auto phase = timestamp_frequency.convert(timestamp);
    return pushChannelsWithPhase(std::move(samples), phase.nanoseconds, sample_rate,
                                 phase.remainder, phase.denominator, timestamp_sample_offset);
}

RxPushStatus RxSampleQueue::pushChannelsWithPhase(std::vector<std::vector<std::int16_t>> samples,
                                                  const std::int64_t time_ns,
                                                  const TimestampRate sample_rate,
                                                  const std::uint64_t remainder,
                                                  const std::uint64_t denominator,
                                                  const std::uint64_t timestamp_sample_offset)
{
    if (samples.size() != channel_count_)
        throw std::invalid_argument("RX sample plane count differs from the configured channels");
    const auto components = samples.front().size();
    if (components == 0 || components % 2 != 0)
        throw std::invalid_argument("RX sample data must contain complete IQ pairs");
    for (const auto &plane : samples)
        if (plane.size() != components)
            throw std::invalid_argument("RX sample planes must have equal lengths");
    if (!sample_rate.valid())
        throw std::invalid_argument("RX sample rate cannot be zero");

    Block block;
    block.channel_samples = std::move(samples);
    block.timestamp_sample_offset = timestamp_sample_offset;
    block.time_ns = time_ns;
    block.sample_rate = sample_rate;
    block.time_remainder = remainder;
    block.time_denominator = denominator;

    bool dropped = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || failed_)
        {
            return RxPushStatus::stopped;
        }
        block.discontinuity_before = next_block_discontinuous_;
        next_block_discontinuous_ = false;
        if (blocks_.size() == capacity_blocks_)
        {
            // Keep latency bounded without blocking the hardware receive worker.
            blocks_.pop_front();
            // The gap belongs before the oldest retained data, including when
            // a partially read block was displaced by a slow consumer.
            if (blocks_.empty())
                block.discontinuity_before = true;
            else
                blocks_.front().discontinuity_before = true;
            ++dropped_blocks_;
            dropped = true;
        }
        blocks_.push_back(std::move(block));
    }
    data_available_.notify_one();
    return dropped ? RxPushStatus::dropped_oldest : RxPushStatus::accepted;
}

std::int64_t RxSampleQueue::blockTime(const Block &block) const
{
    // offset counts complex samples, which advance RF time by one tick each.
    if (block.offset > std::numeric_limits<std::uint64_t>::max() - block.timestamp_sample_offset)
        throw std::overflow_error("RX timestamp sample offset does not fit uint64_t");
    const auto offset_phase = block.sample_rate.convert(block.timestamp_sample_offset +
                                                        static_cast<std::uint64_t>(block.offset));
    const auto offset_ns = offset_phase.nanoseconds;
    if (offset_ns > 0 &&
        block.time_ns > std::numeric_limits<std::int64_t>::max() - offset_ns)
    {
        throw std::overflow_error("RX timestamp does not fit in nanoseconds");
    }
    auto time_ns = block.time_ns + offset_ns;
    if (block.time_remainder != 0 && offset_phase.remainder != 0)
    {
        // Carry one nanosecond exactly when the two fractional phases sum to >= 1.
        // Compare against ceil((rate - offset_phase) * denominator / rate).
        const auto threshold =
            detail::multiplyDivide(offset_phase.denominator - offset_phase.remainder,
                                   block.time_denominator, offset_phase.denominator);
        const auto minimum_phase = threshold.quotient + (threshold.remainder != 0 ? 1 : 0);
        if (block.time_remainder >= minimum_phase)
        {
            if (time_ns == std::numeric_limits<std::int64_t>::max())
                throw std::overflow_error("RX timestamp does not fit in nanoseconds");
            ++time_ns;
        }
    }
    return time_ns;
}

RxReadResult RxSampleQueue::read(
    std::int16_t *output,
    const std::size_t requested_samples,
    const std::chrono::microseconds timeout)
{
    if (channel_count_ != 1)
        throw std::logic_error("use readChannels for a multi-channel RX queue");
    std::int16_t *outputs[]{output};
    return readChannels(outputs, requested_samples, timeout);
}

RxReadResult RxSampleQueue::readChannels(std::int16_t *const *outputs,
                                         const std::size_t requested_samples,
                                         const std::chrono::microseconds timeout)
{
    if (requested_samples != 0)
    {
        if (outputs == nullptr)
            throw std::invalid_argument("RX outputs cannot be null");
        for (std::size_t channel = 0; channel < channel_count_; ++channel)
            if (outputs[channel] == nullptr)
                throw std::invalid_argument("RX output cannot be null");
    }
    if (timeout.count() < 0)
    {
        throw std::invalid_argument("RX timeout cannot be negative");
    }
    if (requested_samples == 0)
    {
        return {RxReadStatus::samples, 0, 0, false};
    }

    std::unique_lock<std::mutex> lock(mutex_);
    // Lifecycle transitions share the wake-up path with newly queued samples.
    const auto ready = [this] {
        return !blocks_.empty() || failed_ || !running_;
    };
    if (!ready() && !data_available_.wait_for(lock, timeout, ready))
    {
        return {RxReadStatus::timeout, 0, 0, false};
    }
    if (blocks_.empty())
    {
        return {failed_ ? RxReadStatus::error : RxReadStatus::stopped, 0, 0, false};
    }
    if (blocks_.front().discontinuity_before)
    {
        blocks_.front().discontinuity_before = false;
        return {RxReadStatus::overflow, 0, 0, false};
    }

    // Capture metadata before consuming blocks; it belongs to the first sample.
    RxReadResult result{RxReadStatus::samples, 0, blockTime(blocks_.front()), true};
    while (result.samples < requested_samples && !blocks_.empty())
    {
        Block &block = blocks_.front();
        if (block.discontinuity_before)
            break;
        const std::size_t block_samples = block.channel_samples.front().size() / 2;
        const std::size_t available = block_samples - block.offset;
        const std::size_t to_copy =
            std::min(available, requested_samples - result.samples);
        for (std::size_t channel = 0; channel < channel_count_; ++channel)
            std::memcpy(outputs[channel] + result.samples * 2,
                        block.channel_samples[channel].data() + block.offset * 2,
                        to_copy * 2 * sizeof(std::int16_t));
        block.offset += to_copy;
        result.samples += to_copy;
        if (block.offset == block_samples)
        {
            blocks_.pop_front();
        }
    }
    return result;
}

bool RxSampleQueue::running() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

std::size_t RxSampleQueue::queuedBlocks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return blocks_.size();
}

std::size_t RxSampleQueue::droppedBlocks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_blocks_;
}

} // namespace soapy_sidekiq
