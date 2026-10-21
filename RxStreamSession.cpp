/**
 * @file RxStreamSession.cpp
 * @brief Implements RX hardware start, worker execution, reads, and teardown.
 */

#include <SoapySidekiq/RxStreamSession.hpp>
#include <SoapySidekiq/TimestampConversion.hpp>

#include <limits>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <utility>

namespace soapy_sidekiq
{

RxStreamSession::RxStreamSession(
    RxStreamBackend &backend,
    const std::size_t queue_capacity_blocks)
    : backend_(backend), queue_(queue_capacity_blocks)
{
}

RxStreamSession::~RxStreamSession()
{
    // std::thread destruction terminates the process if it is still joinable.
    shutdown();
}

int RxStreamSession::activate(const RxSessionConfiguration &configuration)
{
    if (configuration.block_samples == 0 ||
        configuration.block_samples > std::numeric_limits<std::size_t>::max() / 2)
    {
        throw std::invalid_argument("RX block sample count must be positive");
    }
    if (!configuration.sample_rate.valid())
    {
        throw std::invalid_argument("RX sample rate must be positive");
    }
    if (!configuration.use_rf_timestamp &&
        configuration.system_timestamp_frequency == 0)
    {
        throw std::invalid_argument("system timestamp frequency must be positive");
    }

    // Serialize start/stop so only one worker can own the backend at a time.
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (active_ || receive_thread_.joinable())
    {
        throw std::logic_error("RX stream session is already active");
    }

    // Do not publish active state until hardware start succeeds.
    const int start_status = backend_.start(configuration.handle, configuration.on_pps);
    if (start_status != 0)
    {
        return start_status;
    }

    active_configuration_ = configuration;
    hardware_overruns_.store(0);
    timestamp_discontinuities_.store(0);
    receive_error_.store(0);
    failed_.store(false);
    active_ = true;
    try
    {
        queue_.start();
        running_.store(true);
        receive_thread_ = std::thread(
            &RxStreamSession::receiveLoop, this, configuration);
    }
    catch (...)
    {
        // Roll hardware back if the host cannot create the worker thread.
        running_.store(false);
        queue_.reset();
        // A failed rollback still owns hardware; a later deactivate can retry.
        try
        {
            // Cleanup must not wait for an external PPS edge.
            const int status = backend_.stop(configuration.handle, false);
            if (status == 0 || status == -ENODEV) active_ = false;
        }
        catch (...) {}
        throw;
    }
    return 0;
}

int RxStreamSession::deactivate(const bool on_pps)
{
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    if (!active_ && !receive_thread_.joinable())
    {
        return 0;
    }

    // Wake stream readers before stopping hardware, which may block briefly.
    running_.store(false);
    queue_.stop();
    int stop_status = 0;
    std::exception_ptr stop_error;
    try
    {
        if (active_)
            stop_status = backend_.stop(active_configuration_.handle, on_pps);
        if (stop_status == 0 || stop_status == -ENODEV) active_ = false;
    }
    catch (...) { stop_error = std::current_exception(); }

    // Keep activation serialized until the old worker exits. It never takes
    // lifecycle_mutex_, so joining here cannot deadlock with receiveLoop.
    if (receive_thread_.joinable()) receive_thread_.join();
    if (stop_error) std::rethrow_exception(stop_error);
    return stop_status;
}

void RxStreamSession::shutdown() noexcept
{
    try
    {
        (void)deactivate(false);
    }
    catch (...)
    {
        // Destructors cannot propagate; still guarantee the worker is joined.
        running_.store(false);
        queue_.stop();
        if (receive_thread_.joinable())
        {
            receive_thread_.join();
        }
    }
}

RxReadResult RxStreamSession::read(
    std::int16_t *output,
    const std::size_t requested_samples,
    const std::chrono::microseconds timeout)
{
    return queue_.read(output, requested_samples, timeout);
}

bool RxStreamSession::running() const
{
    return running_.load();
}

bool RxStreamSession::failed() const
{
    return failed_.load();
}

std::size_t RxStreamSession::hardwareOverruns() const
{
    return hardware_overruns_.load();
}

std::size_t RxStreamSession::timestampDiscontinuities() const
{
    return timestamp_discontinuities_.load();
}

int RxStreamSession::receiveError() const
{
    return receive_error_.load();
}

void RxStreamSession::receiveLoop(RxSessionConfiguration configuration) noexcept
{
    bool have_last_timestamp = false;
    std::uint64_t last_timestamp = 0;

    try
    {
        while (running_.load())
        {
            auto result = backend_.receive();
            if (!running_.load())
            {
                break;
            }
            if (result.status == RxReceiveStatus::no_data)
            {
                std::this_thread::yield();
                continue;
            }
            if (result.status == RxReceiveStatus::overrun)
            {
                hardware_overruns_.fetch_add(1);
                queue_.markDiscontinuity();
                continue;
            }
            if (result.status == RxReceiveStatus::error)
            {
                receive_error_.store(result.error_code);
                failed_.store(true);
                running_.store(false);
                queue_.fail();
                break;
            }
            if (result.handle != configuration.handle)
            {
                // A card may deliver another configured handle to this call.
                continue;
            }
            if (result.samples.size() != configuration.block_samples * 2)
            {
                receive_error_.store(-1);
                failed_.store(true);
                running_.store(false);
                queue_.fail();
                break;
            }

            // Retain valid samples while marking the boundary before them.
            if (have_last_timestamp)
            {
                const std::uint64_t expected =
                    last_timestamp + configuration.block_samples;
                if (result.rf_timestamp != expected)
                {
                    timestamp_discontinuities_.fetch_add(1);
                    queue_.markDiscontinuity();
                }
            }
            have_last_timestamp = true;
            last_timestamp = result.rf_timestamp;

            const std::uint64_t timestamp = configuration.use_rf_timestamp
                ? result.rf_timestamp
                : result.system_timestamp;
            const TimestampRate frequency =
                configuration.use_rf_timestamp
                    ? configuration.sample_rate
                    : TimestampRate(configuration.system_timestamp_frequency);
            queue_.pushTicks(std::move(result.samples), timestamp, frequency,
                             configuration.sample_rate);
        }
    }
    catch (...)
    {
        // Translate every worker exception into a stable reader-visible error.
        receive_error_.store(-1);
        failed_.store(true);
        running_.store(false);
        queue_.fail();
    }
}

} // namespace soapy_sidekiq
