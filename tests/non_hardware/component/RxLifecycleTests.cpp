/**
 * @file RxLifecycleTests.cpp
 * @brief Exercises production RX lifecycle methods with an injected worker failure.
 */

#include "TestHarness.hpp"
#include <SoapySidekiq/RxStreamSession.hpp>
#include <memory>
#include <vector>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <thread>

#define SOAPY_SDR_RX 1
#define SOAPY_SDR_TX 0
#define SOAPY_SDR_HAS_TIME 4
#define SOAPY_SDR_STREAM_ERROR -2
#define SOAPY_SDR_NOT_SUPPORTED -1
#define SOAPY_SDR_TIMEOUT -3
#define SOAPY_SDR_OVERFLOW -4
#define DEFAULT_NUM_BUFFERS 1
#define SoapySDR_logf(...) ((void)0)
#define SoapySDR_log(...) ((void)0)
namespace SoapySDR { struct Stream {}; }
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
struct skiq_tx_block_t {};
void skiq_tx_block_free(skiq_tx_block_t *block) { std::free(block); }

namespace
{
/** Model hardware ownership independently of the worker's running flag. */
struct FakeSdk
{
    bool streaming{};
    int stop_status{};
    unsigned starts{}, stops{};
    bool stop_on_pps{}, provide_packet{}, delivered{};
    bool provide_overrun{}, overrun_delivered{};
    double actual_rate{20000000};
    std::uint64_t system_frequency{1000000}, rf_timestamp{1}, system_timestamp{1};
    unsigned rate_reads{}, frequency_reads{};
    unsigned rate_handle{};
    std::uint32_t configured_rate{}, configured_bandwidth{}, actual_bandwidth{};
    bool omit_rate{};
    int rate_status{}, frequency_status{};
} sdk;
}

int skiq_read_rx_sample_rate_and_bandwidth(std::uint8_t, skiq_rx_hdl_t handle,
                                           std::uint32_t *configured_rate, double *actual_rate,
                                           std::uint32_t *configured_bandwidth,
                                           std::uint32_t *actual_bandwidth)
{
    ++sdk.rate_reads;
    sdk.rate_handle = handle;
    *configured_rate = sdk.configured_rate;
    if (!sdk.omit_rate)
        *actual_rate = sdk.actual_rate;
    *configured_bandwidth = sdk.configured_bandwidth;
    *actual_bandwidth = sdk.actual_bandwidth;
    return sdk.rate_status;
}
int skiq_read_sys_timestamp_freq(std::uint8_t, std::uint64_t *frequency)
{
    ++sdk.frequency_reads;
    *frequency = sdk.system_frequency;
    return sdk.frequency_status;
}
int skiq_start_rx_streaming(std::uint8_t, skiq_rx_hdl_t)
{
    if (sdk.streaming) return -EBUSY;
    sdk.streaming = true;
    ++sdk.starts;
    return 0;
}
int skiq_stop_rx_streaming(std::uint8_t, skiq_rx_hdl_t)
{
    ++sdk.stops;
    sdk.stop_on_pps = false;
    if (sdk.stop_status != 0) return sdk.stop_status;
    if (!sdk.streaming) return -ENODEV;
    sdk.streaming = false;
    return 0;
}
int skiq_start_rx_streaming_on_1pps(std::uint8_t card, skiq_rx_hdl_t handle, int)
{ return skiq_start_rx_streaming(card, handle); }
int skiq_stop_rx_streaming_on_1pps(std::uint8_t card, skiq_rx_hdl_t handle, int)
{
    const int status = skiq_stop_rx_streaming(card, handle);
    sdk.stop_on_pps = true;
    return status;
}
int skiq_stop_tx_streaming(std::uint8_t, skiq_tx_hdl_t) { return 0; }
int skiq_stop_tx_streaming_on_1pps(std::uint8_t, skiq_tx_hdl_t, int) { return 0; }

/** Route the real session to the same fake hardware used by the device fixture. */
class FixtureBackend final : public soapy_sidekiq::RxStreamBackend
{
public:
    int start(std::uint32_t handle, bool pps) override
    { return pps ? skiq_start_rx_streaming_on_1pps(0, handle, 0) : skiq_start_rx_streaming(0, handle); }
    int stop(std::uint32_t handle, bool pps) override
    { return pps ? skiq_stop_rx_streaming_on_1pps(0, handle, 0) : skiq_stop_rx_streaming(0, handle); }
    soapy_sidekiq::RxReceiveResult receive() override
    {
        if (sdk.provide_overrun && !sdk.overrun_delivered)
        {
            sdk.overrun_delivered = true;
            return {soapy_sidekiq::RxReceiveStatus::overrun, 0, {}, 0, 0, 0};
        }
        if (!sdk.provide_packet)
            throw std::runtime_error("injected receive failure");
        if (sdk.delivered)
            return {};
        sdk.delivered = true;
        return {soapy_sidekiq::RxReceiveStatus::success,
                sdk.rate_handle,
                {1, 2, 3, 4},
                sdk.rf_timestamp,
                sdk.system_timestamp,
                0};
    }
};

/** Supply the device fields used by the extracted production lifecycle methods. */
class SoapySidekiq
{
public:
    struct StreamHandle
    {
        int direction{SOAPY_SDR_RX};
        skiq_rx_hdl_t rx_handle{};
        skiq_tx_hdl_t tx_handle{};
        std::size_t channel{};
    };
    std::uint8_t card{};
    bool tx_start_finished{}, tx_stream_active{}, rfTimeSource{true};
    std::mutex tx_mutex;
    std::mutex tx_buf_mutex;
    std::condition_variable tx_cv, tx_buf_cv;
    bool tx_writes_stopping{true};
    std::size_t active_tx_writers{}, active_tx_status_readers{};
    void stopTxWrites();
    std::uint32_t rx_payload_size_in_words{2}, current_tx_block_size{252};
    bool rxUseShort{true};
    double maxValue{2048};
    std::uint64_t sys_freq{1000000};
    std::vector<std::uint32_t> rx_sample_rates{20000000};
    StreamHandle *active_rx_stream{}, *active_tx_stream{};
    FixtureBackend backend;
    std::unique_ptr<soapy_sidekiq::RxStreamSession> rx_session;
    std::thread _tx_streaming_thread;
    skiq_tx_block_t *p_tx_block[DEFAULT_NUM_BUFFERS]{};

    SoapySidekiq()
    {
        sdk = {};
        rx_session = std::make_unique<soapy_sidekiq::RxStreamSession>(backend);
    }
    ~SoapySidekiq()
    {
        rx_session.reset();
        delete active_rx_stream;
    }
    SoapySDR::Stream *setupRx()
    {
        REQUIRE(active_rx_stream == nullptr);
        active_rx_stream = new StreamHandle;
        return reinterpret_cast<SoapySDR::Stream *>(active_rx_stream);
    }
    void awaitWorkerFailure()
    {
        std::int16_t output[2]{};
        REQUIRE(rx_session->read(output, 1, std::chrono::seconds(1)).status ==
                soapy_sidekiq::RxReadStatus::error);
        REQUIRE(!rx_session->running());
    }
    StreamHandle *findStreamHandle(SoapySDR::Stream *) const noexcept;
    std::size_t getStreamMTU(SoapySDR::Stream *) const;
    int readStream(SoapySDR::Stream *, void *const *, std::size_t, int &, long long &, long);
    int activateStream(SoapySDR::Stream *, int, long long, std::size_t);
    int deactivateStream(SoapySDR::Stream *, int, long long);
    void closeStream(SoapySDR::Stream *);
};

#include "StreamHandleLookup.inc"
#include "StreamMtuMethod.inc"
#include "StreamReadMethod.inc"

// Production close/deactivate and RX activation, using the real session worker.
#include "RxLifecycleMethods.inc"
#include "TxStopWritesMethod.inc"

TEST_CASE("closing a failed RX worker stops hardware and permits reactivation")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    REQUIRE(sdk.streaming);
    device.closeStream(stream);
    REQUIRE(!sdk.streaming);
    REQUIRE(!device.rx_session->running());
    REQUIRE_EQ(sdk.stops, 1u);
    stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    device.closeStream(stream);
    REQUIRE_EQ(sdk.starts, 2u);
    REQUIRE_EQ(sdk.stops, 2u);
}

TEST_CASE("production readStream reports hardware loss before returning post-gap CS16 or CF32")
{
    for (const bool cs16 : {true, false})
    {
        SoapySidekiq device;
        sdk.provide_packet = true;
        sdk.provide_overrun = true;
        device.rxUseShort = cs16;
        auto *stream = device.setupRx();
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
        std::int16_t shorts[4]{99, 99, 99, 99};
        float floats[4]{99, 99, 99, 99};
        void *buffers[]{cs16 ? static_cast<void *>(shorts) : static_cast<void *>(floats)};
        int flags = 123;
        long long time = 456;
        REQUIRE_EQ(device.readStream(stream, buffers, 2, flags, time, 250000), SOAPY_SDR_OVERFLOW);
        REQUIRE_EQ(flags, 0);
        REQUIRE_EQ(time, 0LL);
        REQUIRE_EQ(shorts[0], std::int16_t{99});
        REQUIRE_EQ(floats[0], 99.0f);
        REQUIRE_EQ(device.readStream(stream, buffers, 2, flags, time, 250000), 2);
        REQUIRE_EQ(flags, SOAPY_SDR_HAS_TIME);
        REQUIRE_EQ(time, 50LL);
        if (cs16)
            REQUIRE_EQ(shorts[0], std::int16_t{1});
        else
            REQUIRE_EQ(floats[0], 1.0f / 2048);
        REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
        device.closeStream(stream);
    }
}

TEST_CASE("failed RX stop preserves ownership for a successful close retry")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    sdk.stop_status = -EIO;
    REQUIRE_THROWS_AS(device.closeStream(stream), std::runtime_error);
    REQUIRE(sdk.streaming);
    REQUIRE_THROWS_AS(device.rx_session->activate({0, 2, 20000000, 1000000, true, false}),
                      std::logic_error);
    REQUIRE(device.active_rx_stream != nullptr);
    sdk.stop_status = 0;
    device.closeStream(stream);
    REQUIRE(!sdk.streaming);
    REQUIRE(!device.rx_session->running());
    REQUIRE_EQ(sdk.stops, 2u);
}

TEST_CASE("RX close handles already-stopped hardware without redundant deactivation")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    sdk.streaming = false;
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    REQUIRE(!device.rx_session->running());
    device.closeStream(stream);
    REQUIRE_EQ(sdk.stops, 1u);
}

TEST_CASE("immediate RX stop is independent of PPS activation")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), 0);
    device.awaitWorkerFailure();
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    REQUIRE(!sdk.stop_on_pps);
    device.closeStream(stream);
}

TEST_CASE("PPS RX stop is independent of immediate activation")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    REQUIRE_EQ(device.deactivateStream(stream, SOAPY_SDR_HAS_TIME, 0), 0);
    REQUIRE(sdk.stop_on_pps);
    device.closeStream(stream);
}

TEST_CASE("closing a failed PPS RX stream requests immediate hardware cleanup")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), 0);
    device.awaitWorkerFailure();
    device.closeStream(stream);
    REQUIRE_EQ(sdk.stops, 1u);
    REQUIRE(!sdk.stop_on_pps);
}

TEST_CASE("session destruction after PPS activation requests immediate hardware cleanup")
{
    {
        SoapySidekiq device;
        auto *stream = device.setupRx();
        REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), 0);
        device.awaitWorkerFailure();
    }
    REQUIRE_EQ(sdk.stops, 1u);
    REQUIRE(!sdk.stop_on_pps);
}

TEST_CASE("RX stream APIs reject null inaccessible and foreign tokens before dereference")
{
    SoapySidekiq device;
    SoapySidekiq other;
    auto *live = device.setupRx();
    auto *foreign = other.setupRx();
    int flags = 123;
    long long timestamp = 456;
    for (auto *invalid : {static_cast<SoapySDR::Stream *>(nullptr),
                          reinterpret_cast<SoapySDR::Stream *>(std::uintptr_t{1}), foreign})
    {
        REQUIRE_EQ(device.activateStream(invalid, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
        REQUIRE_EQ(device.deactivateStream(invalid, 0, 0), SOAPY_SDR_STREAM_ERROR);
        REQUIRE_EQ(device.readStream(invalid, nullptr, 1, flags, timestamp, 0),
                   SOAPY_SDR_STREAM_ERROR);
        REQUIRE_THROWS_AS(device.getStreamMTU(invalid), std::invalid_argument);
        REQUIRE_THROWS_AS(device.closeStream(invalid), std::invalid_argument);
    }
    REQUIRE_EQ(sdk.starts + sdk.stops, 0u);
    REQUIRE_EQ(flags, 123);
    REQUIRE_EQ(timestamp, 456LL);
    REQUIRE_EQ(device.getStreamMTU(live), std::size_t{2});
}
TEST_CASE("closed RX tokens are rejected without accessing freed storage")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    device.closeStream(stream);
    int flags = 0;
    long long timestamp = 0;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.readStream(stream, nullptr, 1, flags, timestamp, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_THROWS_AS(device.getStreamMTU(stream), std::invalid_argument);
    REQUIRE_THROWS_AS(device.closeStream(stream), std::invalid_argument);
}

TEST_CASE("stream MTU reports positive sizes and rejects an invalid direction")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.getStreamMTU(stream), std::size_t{2});
    device.active_rx_stream->direction = SOAPY_SDR_TX;
    REQUIRE_EQ(device.getStreamMTU(stream), std::size_t{252});
    device.active_rx_stream->direction = 99;
    REQUIRE_THROWS_AS(device.getStreamMTU(stream), std::invalid_argument);
    device.active_rx_stream->direction = SOAPY_SDR_RX;
}

TEST_CASE("RX activation refreshes actual fractional rate instead of the stale cache")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.provide_packet = true;
    sdk.actual_rate = 1.5;
    device.active_rx_stream->channel = 1;
    device.active_rx_stream->rx_handle = 7;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    std::int16_t output[2]{};
    const auto first = device.rx_session->read(output, 1, std::chrono::seconds(1));
    REQUIRE_EQ(first.time_ns, std::int64_t{666666666});
    const auto second = device.rx_session->read(output, 1, std::chrono::seconds(1));
    REQUIRE_EQ(second.time_ns, std::int64_t{1333333333});
    REQUIRE_EQ(sdk.rate_handle, 7u);
    REQUIRE_EQ(sdk.rate_reads, 1u);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
}
TEST_CASE("RX activation refreshes the selected system clock before starting hardware")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.provide_packet = true;
    sdk.system_frequency = 3;
    device.rfTimeSource = false;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    std::int16_t output[2]{};
    REQUIRE_EQ(device.rx_session->read(output, 1, std::chrono::seconds(1)).time_ns,
               std::int64_t{333333333});
    REQUIRE_EQ(sdk.frequency_reads, 1u);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
}
TEST_CASE("RX activation validates refreshed rates before any hardware start")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.rate_status = -5;
    REQUIRE_THROWS_AS(device.activateStream(stream, 0, 0, 0), std::runtime_error);
    sdk.rate_status = 0;
    sdk.actual_rate = 0;
    REQUIRE_THROWS_AS(device.activateStream(stream, 0, 0, 0), std::invalid_argument);
    sdk.actual_rate = 20000000;
    device.rfTimeSource = false;
    sdk.frequency_status = -6;
    REQUIRE_THROWS_AS(device.activateStream(stream, 0, 0, 0), std::runtime_error);
    sdk.frequency_status = 0;
    sdk.system_frequency = 0;
    REQUIRE_THROWS_AS(device.activateStream(stream, 0, 0, 0), std::invalid_argument);
    REQUIRE_EQ(sdk.starts, 0u);
}

TEST_CASE("RX timestamp setup accepts actual rate without configured rate or bandwidth")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.provide_packet = true;
    sdk.actual_rate = 1.5;
    sdk.configured_rate = sdk.configured_bandwidth = sdk.actual_bandwidth = 0;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    std::int16_t output[2]{};
    REQUIRE_EQ(device.rx_session->read(output, 1, std::chrono::seconds(1)).time_ns,
               std::int64_t{666666666});
    REQUIRE_EQ(sdk.starts, 1u);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
}
TEST_CASE("RX timestamp setup rejects omitted actual rate before hardware start")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.omit_rate = true;
    REQUIRE_THROWS_AS(device.activateStream(stream, 0, 0, 0), std::invalid_argument);
    REQUIRE_EQ(sdk.starts, 0u);
}

TEST_CASE("activation rejects unsupported flags bursts and absolute times before starting")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        SoapySidekiq device;
        auto *stream = device.setupRx();
        device.active_rx_stream->direction = direction;
        for (const int flags : {1, 2, SOAPY_SDR_HAS_TIME | 2, -1})
            REQUIRE_EQ(device.activateStream(stream, flags, 0, 0), SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 1), SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 1),
                   SOAPY_SDR_NOT_SUPPORTED);
        for (const long long time : {-1LL, 1LL, 1000000000LL})
            REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, time, 0),
                       SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(sdk.starts + sdk.stops + sdk.rate_reads + sdk.frequency_reads, 0u);
        device.active_rx_stream->direction = SOAPY_SDR_RX;
    }
}
TEST_CASE("RX stop rejects unsupported options without stopping the active session")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.provide_packet = true;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    for (const int flags : {1, SOAPY_SDR_HAS_TIME | 2, -1})
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE_EQ(device.deactivateStream(stream, SOAPY_SDR_HAS_TIME, 1), SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE_EQ(sdk.stops, 0u);
    REQUIRE(device.rx_session->running());
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
}
TEST_CASE("immediate RX activation ignores timeNs when HAS_TIME is absent")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.provide_packet = true;
    REQUIRE_EQ(device.activateStream(stream, 0, 123, 0), 0);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 123), 0);
}

TEST_CASE("failed RX deactivation returns a stream error and preserves hardware stop retry")
{
    for (const int flags : {0, SOAPY_SDR_HAS_TIME})
    {
        SoapySidekiq device;
        auto *stream = device.setupRx();
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
        device.awaitWorkerFailure();
        auto *const owner = device.active_rx_stream;
        sdk.stop_status = -EIO;
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), SOAPY_SDR_STREAM_ERROR);
        REQUIRE(device.active_rx_stream == owner);
        REQUIRE(sdk.streaming);
        REQUIRE_EQ(sdk.stop_on_pps, flags == SOAPY_SDR_HAS_TIME);
        REQUIRE_THROWS_AS(device.closeStream(stream), std::runtime_error);
        REQUIRE(device.active_rx_stream == owner);
        REQUIRE(sdk.streaming);
        sdk.stop_status = 0;
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), 0);
        REQUIRE(!sdk.streaming);
        device.closeStream(stream);
        REQUIRE(device.active_rx_stream == nullptr);
    }
}
