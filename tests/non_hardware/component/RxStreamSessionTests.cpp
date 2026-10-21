/**
 * @file RxStreamSessionTests.cpp
 * @brief Verifies RX worker lifecycle, errors, timing, filtering, and cleanup.
 */

#include "TestHarness.hpp"

#include <SoapySidekiq/RxStreamSession.hpp>
#include <SoapySidekiq/ScopeExit.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using soapy_sidekiq::RxReadStatus;
using soapy_sidekiq::RxReceiveResult;
using soapy_sidekiq::RxReceiveStatus;
using soapy_sidekiq::RxSessionConfiguration;
using soapy_sidekiq::RxStreamBackend;
using soapy_sidekiq::RxStreamSession;

namespace
{

/** Blocking backend fake used to exercise real worker wakeup and join paths. */
class FakeRxBackend final : public RxStreamBackend
{
public:
    /** @copydoc RxStreamBackend::start */
    int start(std::uint32_t handle, bool on_pps) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++start_calls;
        started_handle = handle;
        started_on_pps = on_pps;
        if (streaming_) return -EBUSY;
        if (start_status == 0)
        {
            streaming_ = true;
        }
        return start_status;
    }

    /** @copydoc RxStreamBackend::stop */
    int stop(std::uint32_t handle, bool on_pps) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stop_calls;
            stopped_handle = handle;
            stopped_on_pps = on_pps;
            if (throw_on_stop) throw std::runtime_error("injected stop exception");
            if (stop_status == 0 || stop_status == -ENODEV) streaming_ = false;
        }
        // Release a receive() blocked on an empty scripted event queue.
        event_available_.notify_all();
        return stop_status;
    }

    /** @copydoc RxStreamBackend::receive */
    RxReceiveResult receive() override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Real SDK receive has a bounded timeout, including after stop failure.
        event_available_.wait_for(lock, 10ms, [this] {
            return !events_.empty() || !streaming_;
        });
        if (events_.empty())
        {
            return {RxReceiveStatus::no_data, 0, {}, 0, 0, 0};
        }
        auto event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

    /**
     * Make one receive result available to the session worker.
     * @param event Owned result appended to the fake's receive queue.
     */
    void enqueue(RxReceiveResult event)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(std::move(event));
        }
        event_available_.notify_one();
    }

    int start_status{};
    int stop_status{};
    bool throw_on_stop{};
    int start_calls{};
    int stop_calls{};
    std::uint32_t started_handle{};
    std::uint32_t stopped_handle{};
    bool started_on_pps{};
    bool stopped_on_pps{};

private:
    std::mutex mutex_;
    std::condition_variable event_available_;
    std::deque<RxReceiveResult> events_;
    bool streaming_{};
};

/**
 * Return a valid two-complex-sample session configuration.
 * @param use_rf_timestamp Select RF rather than system timestamps.
 * @param on_pps Select PPS-synchronized backend start.
 */
RxSessionConfiguration configuration(
    const bool use_rf_timestamp = true,
    const bool on_pps = false)
{
    return {7, 2, 20000000, 1000000, use_rf_timestamp, on_pps};
}

/**
 * Build a successful receive event with explicit handle and timestamps.
 * @param handle Hardware handle reported by the fake backend.
 * @param values Interleaved CS16 values transferred into the result.
 * @param rf_timestamp RF sample-counter timestamp.
 * @param system_timestamp System-counter timestamp.
 */
RxReceiveResult samples(
    const std::uint32_t handle,
    std::vector<std::int16_t> values,
    const std::uint64_t rf_timestamp,
    const std::uint64_t system_timestamp = 0)
{
    return {
        RxReceiveStatus::success,
        handle,
        std::move(values),
        rf_timestamp,
        system_timestamp,
        0};
}

} // namespace

TEST_CASE("RX session starts receives and stops through its backend")
{
    FakeRxBackend backend;
    RxStreamSession session(backend, 4);
    REQUIRE_EQ(session.activate(configuration(true, true)), 0);
    REQUIRE_EQ(backend.start_calls, 1);
    REQUIRE_EQ(backend.started_handle, std::uint32_t{7});
    REQUIRE(backend.started_on_pps);

    backend.enqueue(samples(7, {1, -1, 2, -2}, 20));
    std::array<std::int16_t, 4> output{};
    const auto result = session.read(output.data(), 2, 250ms);
    REQUIRE(result.status == RxReadStatus::samples);
    REQUIRE_EQ(result.samples, std::size_t{2});
    REQUIRE_EQ(result.time_ns, std::int64_t{1000});
    REQUIRE_EQ(output[0], std::int16_t{1});
    REQUIRE_EQ(output[3], std::int16_t{-2});

    REQUIRE_EQ(session.deactivate(true), 0);
    REQUIRE_EQ(backend.stop_calls, 1);
    REQUIRE_EQ(backend.stopped_handle, std::uint32_t{7});
    REQUIRE(backend.stopped_on_pps);
}

TEST_CASE("RX session supports system timestamp selection")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration(false)), 0);
    backend.enqueue(samples(7, {1, 2, 3, 4}, 99, 3));

    std::array<std::int16_t, 4> output{};
    const auto result = session.read(output.data(), 2, 250ms);
    REQUIRE_EQ(result.time_ns, std::int64_t{3000});
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX session ignores blocks for a different handle")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.enqueue(samples(8, {9, 9, 9, 9}, 10));
    backend.enqueue(samples(7, {1, 2, 3, 4}, 12));

    std::array<std::int16_t, 4> output{};
    const auto result = session.read(output.data(), 2, 250ms);
    REQUIRE_EQ(result.samples, std::size_t{2});
    REQUIRE_EQ(output[0], std::int16_t{1});
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX session continues receiving after a no-data result")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.enqueue({RxReceiveStatus::no_data, 0, {}, 0, 0, 0});
    backend.enqueue(samples(7, {1, 2, 3, 4}, 12));

    std::array<std::int16_t, 4> output{};
    REQUIRE_EQ(session.read(output.data(), 2, 250ms).samples, std::size_t{2});
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX session counts overruns and timestamp discontinuities")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.enqueue({RxReceiveStatus::overrun, 0, {}, 0, 0, 0});
    backend.enqueue(samples(7, {1, 2, 3, 4}, 100));
    backend.enqueue(samples(7, {5, 6, 7, 8}, 200));

    std::array<std::int16_t, 4> first{};
    std::array<std::int16_t, 4> second{};
    REQUIRE(session.read(first.data(), 2, 250ms).status == RxReadStatus::overflow);
    REQUIRE_EQ(session.read(first.data(), 2, 250ms).samples, std::size_t{2});
    REQUIRE(session.read(second.data(), 2, 250ms).status == RxReadStatus::overflow);
    REQUIRE_EQ(session.read(second.data(), 2, 250ms).samples, std::size_t{2});
    REQUIRE_EQ(session.hardwareOverruns(), std::size_t{1});
    REQUIRE_EQ(session.timestampDiscontinuities(), std::size_t{1});
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX session propagates fatal receive errors to blocked reads")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.enqueue({RxReceiveStatus::error, 0, {}, 0, 0, -77});

    std::array<std::int16_t, 2> output{};
    const auto result = session.read(output.data(), 1, 250ms);
    REQUIRE(result.status == RxReadStatus::error);
    REQUIRE(session.failed());
    REQUIRE_EQ(session.receiveError(), -77);
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RF sample gaps are reported even when the client selects system timestamps")
{
    for (const bool rf : {true, false})
    {
        FakeRxBackend backend;
        RxStreamSession session(backend);
        REQUIRE_EQ(session.activate(configuration(rf)), 0);
        backend.enqueue(samples(7, {1, 2, 3, 4}, 100, 10));
        std::int16_t output[4]{};
        REQUIRE_EQ(session.read(output, 2, 250ms).samples, std::size_t{2});
        backend.enqueue(samples(7, {5, 6, 7, 8}, 200, 20));
        REQUIRE(session.read(output, 2, 250ms).status == RxReadStatus::overflow);
        const auto resumed = session.read(output, 2, 250ms);
        REQUIRE_EQ(resumed.samples, std::size_t{2});
        REQUIRE_EQ(resumed.time_ns, rf ? std::int64_t{10000} : std::int64_t{20000});
        REQUIRE_EQ(output[0], std::int16_t{5});
        REQUIRE_EQ(session.deactivate(), 0);
    }
}

TEST_CASE("RX session rejects malformed hardware block sizes")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.enqueue(samples(7, {1, 2}, 0));

    std::array<std::int16_t, 2> output{};
    const auto result = session.read(output.data(), 1, 250ms);
    REQUIRE(result.status == RxReadStatus::error);
    REQUIRE(session.failed());
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX start failures do not leave an active worker")
{
    FakeRxBackend backend;
    backend.start_status = -5;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), -5);
    REQUIRE(!session.running());
    REQUIRE_EQ(backend.stop_calls, 0);

    backend.start_status = 0;
    REQUIRE_EQ(session.activate(configuration()), 0);
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX stop failures are returned after the worker exits")
{
    FakeRxBackend backend;
    backend.stop_status = -9;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    REQUIRE_EQ(session.deactivate(), -9);
    REQUIRE(!session.running());
}

TEST_CASE("RX session destruction unblocks a waiting receive")
{
    FakeRxBackend backend;
    {
        RxStreamSession session(backend);
        REQUIRE_EQ(session.activate(configuration()), 0);
        // Destructor must stop the fake so its blocked receive() can return.
    }
    REQUIRE_EQ(backend.stop_calls, 1);
}

TEST_CASE("RX deactivation wakes a blocked stream reader")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    std::array<std::int16_t, 2> output{};
    auto reader = std::async(std::launch::async, [&] {
        return session.read(output.data(), 1, 5s);
    });

    // Deactivation must notify the queue instead of waiting for five seconds.
    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE(reader.wait_for(250ms) == std::future_status::ready);
    REQUIRE(reader.get().status == RxReadStatus::stopped);
}

TEST_CASE("RX session validates configuration and repeated activation")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    auto invalid = configuration();
    invalid.block_samples = 0;
    REQUIRE_THROWS_AS(session.activate(invalid), std::invalid_argument);
    invalid = configuration();
    invalid.sample_rate = 0;
    REQUIRE_THROWS_AS(session.activate(invalid), std::invalid_argument);
    invalid = configuration(false);
    invalid.system_timestamp_frequency = 0;
    REQUIRE_THROWS_AS(session.activate(invalid), std::invalid_argument);

    REQUIRE_EQ(session.activate(configuration()), 0);
    REQUIRE_THROWS_AS(session.activate(configuration()), std::logic_error);
    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE_EQ(session.activate(configuration()), 0);
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX stop failure preserves hardware ownership until cleanup is retried")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.stop_status = -EIO;
    REQUIRE_EQ(session.deactivate(), -EIO);
    REQUIRE_THROWS_AS(session.activate(configuration()), std::logic_error);
    backend.stop_status = 0;
    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE_EQ(backend.stop_calls, 2);
    REQUIRE_EQ(session.activate(configuration()), 0);
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX stop exceptions join the worker and preserve retryable ownership")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.throw_on_stop = true;
    REQUIRE_THROWS_AS(session.deactivate(), std::runtime_error);
    REQUIRE(!session.running());
    REQUIRE_THROWS_AS(session.activate(configuration()), std::logic_error);
    backend.throw_on_stop = false;
    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE_EQ(backend.stop_calls, 2);
}

TEST_CASE("RX session partial reads retain RF and system timestamp phase")
{
    for (const bool rf : {true, false})
    {
        FakeRxBackend backend;
        RxStreamSession session(backend);
        auto config = configuration(rf);
        config.block_samples = 10;
        config.sample_rate = 122880000;
        config.system_timestamp_frequency = 40960000;
        REQUIRE_EQ(session.activate(config), 0);
        backend.enqueue(samples(7, std::vector<std::int16_t>(20), 1, 1));
        std::array<std::int16_t, 20> output{};
        REQUIRE_EQ(session.read(output.data(), 7, 250ms).samples, std::size_t{7});
        REQUIRE_EQ(session.read(output.data(), 1, 0us).time_ns,
                   rf ? std::int64_t{65} : std::int64_t{81});
        REQUIRE_EQ(session.deactivate(), 0);
    }
}

TEST_CASE("RX activation waits until the previous worker has finished joining")
{
    class HeldReceiveBackend final : public RxStreamBackend
    {
    public:
        std::promise<void> entered, stop_called, release;
        std::shared_future<void> released{release.get_future().share()};
        std::atomic<bool> first_receive{true}, first_stop{true};
        int start(std::uint32_t, bool) override { return 0; }
        int stop(std::uint32_t, bool) override
        {
            if (first_stop.exchange(false)) stop_called.set_value();
            return 0;
        }
        RxReceiveResult receive() override
        {
            if (first_receive.exchange(false))
            {
                entered.set_value();
                released.wait();
            }
            return {};
        }
    } backend;
    RxStreamSession session(backend);
    bool released = false;
    soapy_sidekiq::ScopeExit release_on_failure([&]() noexcept {
        if (!released) backend.release.set_value();
    });
    REQUIRE_EQ(session.activate(configuration()), 0);
    REQUIRE(backend.entered.get_future().wait_for(1s) == std::future_status::ready);
    auto stop = std::async(std::launch::async, [&] { return session.deactivate(); });
    backend.stop_called.get_future().wait();
    std::promise<void> attempting;
    auto start = std::async(std::launch::async, [&] {
        attempting.set_value();
        return session.activate(configuration());
    });
    attempting.get_future().wait();
    const auto state = start.wait_for(30ms);
    backend.release.set_value();
    released = true;
    // The old implementation could start another worker while stop was joining.
    // Stop that worker before waiting on the first stop, so regression failure
    // reports an assertion rather than leaving an async destructor blocked.
    if (state == std::future_status::ready) (void)session.deactivate();
    REQUIRE_EQ(stop.get(), 0);
    REQUIRE_EQ(start.get(), 0);
    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE(state == std::future_status::timeout);
}

TEST_CASE("RX stop retry may choose immediate cleanup after a PPS stop failure")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration(false, false)), 0);
    backend.stop_status = -EIO;
    REQUIRE_EQ(session.deactivate(true), -EIO);
    REQUIRE(backend.stopped_on_pps);
    REQUIRE_THROWS_AS(session.activate(configuration()), std::logic_error);
    backend.stop_status = 0;
    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE(!backend.stopped_on_pps);
    REQUIRE_EQ(backend.stop_calls, 2);
}
