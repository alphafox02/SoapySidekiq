/**
 * @file RxStreamSession.hpp
 * @brief Declares the hardware-independent RX worker and lifecycle controller.
 */

#pragma once

#include <SoapySidekiq/RxSampleQueue.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace soapy_sidekiq
{

/** Result categories returned by an RX hardware backend. */
enum class RxReceiveStatus
{
    success,
    no_data,
    overrun,
    error
};

/** One normalized receive result, independent of the Sidekiq SDK types. */
struct RxReceiveResult
{
    /** Normalized outcome of the backend receive call. */
    RxReceiveStatus status{RxReceiveStatus::no_data};
    /** Hardware RX handle associated with a successful block. */
    std::uint32_t handle{};
    /** Owned interleaved CS16 IQ values. */
    std::vector<std::int16_t> samples;
    /** RF sample-counter timestamp supplied by the hardware. */
    std::uint64_t rf_timestamp{};
    /** System-counter timestamp supplied by the hardware. */
    std::uint64_t system_timestamp{};
    /** Original backend status for an error result. */
    int error_code{};
};

/** Hardware boundary used by RxStreamSession and its non-hardware tests. */
class RxStreamBackend
{
  public:
    virtual ~RxStreamBackend() = default;

    /**
     * Start an RX handle, optionally synchronized to PPS.
     * @param handle Portable numeric RX handle.
     * @param on_pps Whether start should wait for the next PPS edge.
     * @return Backend status code; zero indicates success.
     */
    virtual int start(std::uint32_t handle, bool on_pps) = 0;

    /**
     * Stop an RX handle using the requested synchronization mode.
     * @param handle Portable numeric RX handle.
     * @param on_pps Whether stop should wait for the next PPS edge.
     * @return Backend status code; zero indicates success.
     */
    virtual int stop(std::uint32_t handle, bool on_pps) = 0;

    /** Start selected handles as one synchronized RX-only operation. */
    virtual int startMulti(const std::vector<std::uint32_t> &handles, bool on_pps)
    {
        return handles.size() == 1 ? start(handles.front(), on_pps) : -ENOTSUP;
    }

    /**
     * Stop selected handles, including retries after a partially successful stop.
     * Zero or -ENODEV must mean every selected handle is inactive. Any other
     * result retains ownership of the full selection for a later retry.
     */
    virtual int stopMulti(const std::vector<std::uint32_t> &handles, bool on_pps)
    {
        return handles.size() == 1 ? stop(handles.front(), on_pps) : -ENOTSUP;
    }

    /**
     * Wait for and normalize the next hardware receive result.
     * @return Owned samples, timestamps, handle, and normalized status.
     */
    virtual RxReceiveResult receive() = 0;
};

/** Immutable settings captured for one active receive session. */
struct RxSessionConfiguration
{
    /** Hardware RX handle accepted by this session. */
    std::uint32_t handle{};
    /** Expected number of complex samples per hardware block. */
    std::size_t block_samples{};
    /** RX sample rate used for RF timestamp conversion. */
    TimestampRate sample_rate{};
    /** System-counter frequency used when RF time is not selected. */
    std::uint64_t system_timestamp_frequency{};
    /** Select RF timestamps instead of system-counter timestamps. */
    bool use_rf_timestamp{true};
    /** Synchronize hardware start to PPS; stopping has its own mode. */
    bool on_pps{};
    /** Requested buffer order; empty retains the single-handle API. */
    std::vector<std::uint32_t> handles{};
};

/**
 * Owns the RX worker lifecycle and transfers normalized hardware blocks into a
 * thread-safe RxSampleQueue. The session always joins its worker before it is
 * destroyed, including after backend failures.
 */
class RxStreamSession
{
  public:
    /**
     * Construct a session around a backend that must outlive this object.
     * @param backend Hardware adapter used by the worker thread.
     * @param queue_capacity_blocks Maximum number of queued hardware blocks.
     * @param channel_count Number of selected channel planes in each read.
     * @throws std::invalid_argument when capacity or channel count is zero.
     */
    explicit RxStreamSession(RxStreamBackend &backend, std::size_t queue_capacity_blocks = 64,
                             std::size_t channel_count = 1);

    /** Stop the backend and join the receive worker if still active. */
    ~RxStreamSession();

    RxStreamSession(const RxStreamSession &) = delete;
    RxStreamSession &operator=(const RxStreamSession &) = delete;

    /**
     * Start hardware and its receive worker.
     * @param configuration Immutable settings captured by the worker.
     * @return Backend start status; zero indicates an active session.
     * @throws std::invalid_argument for unusable sizes or timestamp frequencies.
     * @throws std::logic_error when a prior session is still active or joinable.
     */
    int activate(const RxSessionConfiguration &configuration);

    /**
     * Stop hardware, wake readers, and join the worker.
     * @param on_pps Wait for PPS when true; false stops immediately.
     * @return Backend stop status, or zero when already inactive.
     * Failed stops retain hardware ownership for retry and block activation.
     * Backend exceptions are rethrown only after joining the worker.
     */
    int deactivate(bool on_pps = false);

    /** Best-effort, non-throwing teardown for destructors and device cleanup. */
    void shutdown() noexcept;

    /**
     * Read queued complex samples with the caller's exact timeout.
     * @param output Destination for interleaved CS16 IQ values.
     * @param requested_samples Maximum number of complex samples to copy.
     * @param timeout Maximum wait for samples or a lifecycle transition.
     * @return Queue result containing status, count, and timestamp metadata.
     */
    RxReadResult read(std::int16_t *output, std::size_t requested_samples,
                      std::chrono::microseconds timeout);

    /** Read equal sample counts into all selected channel planes. */
    RxReadResult readChannels(std::int16_t *const *outputs, std::size_t requested_samples,
                              std::chrono::microseconds timeout);

    /** @return True while the receive worker should continue running. */
    bool running() const;

    /** @return True after a fatal receive or block-validation error. */
    bool failed() const;

    /** @return Hardware overrun notifications seen in the current session. */
    std::size_t hardwareOverruns() const;

    /** @return Non-contiguous RF timestamps seen in the current session. */
    std::size_t timestampDiscontinuities() const;

    /** @return Backend error code saved by the receive worker, or zero. */
    int receiveError() const;

  private:
    /**
     * Worker entry point; all failures are translated into session state.
     * @param configuration Immutable settings copied into the worker thread.
     */
    void receiveLoop(RxSessionConfiguration configuration) noexcept;

    /** Non-owning backend reference; the caller guarantees its lifetime. */
    RxStreamBackend &backend_;
    /** Synchronized producer/consumer handoff owned by this session. */
    RxSampleQueue queue_;
    const std::size_t queue_capacity_, channel_count_;
    mutable std::mutex lifecycle_mutex_;
    std::thread receive_thread_;
    std::atomic<bool> running_{};
    std::atomic<bool> failed_{};
    std::atomic<std::size_t> hardware_overruns_{};
    std::atomic<std::size_t> timestamp_discontinuities_{};
    std::atomic<int> receive_error_{};
    RxSessionConfiguration active_configuration_{};
    /** Hardware ownership, retained after failed stops; guarded by lifecycle_mutex_. */
    bool active_{};
};

} // namespace soapy_sidekiq
