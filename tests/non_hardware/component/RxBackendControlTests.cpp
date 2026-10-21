/**
 * @file RxBackendControlTests.cpp
 * @brief Checks production RX-only SDK starts and partial-stop ownership.
 */
#include "TestHarness.hpp"
#include <SoapySidekiq/SidekiqRxBackend.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <utility>
#include <vector>

enum skiq_rx_hdl_t
{
    skiq_rx_hdl_A1,
    skiq_rx_hdl_A2,
    skiq_rx_hdl_B1,
    skiq_rx_hdl_end
};
enum skiq_trigger_src_t
{
    skiq_trigger_src_immediate,
    skiq_trigger_src_1pps
};
namespace
{
struct State
{
    std::vector<skiq_rx_hdl_t> streaming{}, stopped{}, started{};
    bool pps{}, synced{};
    int read_status{}, stop_status{};
    unsigned queries{};
} sdk;
} // namespace
int skiq_start_rx_streaming(std::uint8_t, skiq_rx_hdl_t handle)
{
    sdk.started = {handle};
    return 0;
}
int skiq_start_rx_streaming_on_1pps(std::uint8_t card, skiq_rx_hdl_t handle, std::uint64_t)
{
    sdk.pps = true;
    return skiq_start_rx_streaming(card, handle);
}
int skiq_stop_rx_streaming(std::uint8_t, skiq_rx_hdl_t handle)
{
    sdk.stopped = {handle};
    return sdk.stop_status;
}
int skiq_stop_rx_streaming_on_1pps(std::uint8_t card, skiq_rx_hdl_t handle, std::uint64_t)
{
    sdk.pps = true;
    return skiq_stop_rx_streaming(card, handle);
}
int skiq_start_rx_streaming_multi_synced(std::uint8_t, skiq_rx_hdl_t *handles, std::uint8_t count)
{
    sdk.synced = true;
    sdk.started.assign(handles, handles + count);
    return 0;
}
int skiq_start_rx_streaming_multi_on_trigger(std::uint8_t, skiq_rx_hdl_t *handles,
                                             std::uint8_t count, skiq_trigger_src_t trigger,
                                             std::uint64_t)
{
    sdk.pps = trigger == skiq_trigger_src_1pps;
    sdk.started.assign(handles, handles + count);
    return 0;
}
int skiq_stop_rx_streaming_multi_immediate(std::uint8_t, skiq_rx_hdl_t *handles, std::uint8_t count)
{
    sdk.stopped.assign(handles, handles + count);
    return sdk.stop_status;
}
int skiq_stop_rx_streaming_multi_on_trigger(std::uint8_t card, skiq_rx_hdl_t *handles,
                                            std::uint8_t count, skiq_trigger_src_t trigger,
                                            std::uint64_t)
{
    sdk.pps = trigger == skiq_trigger_src_1pps;
    return skiq_stop_rx_streaming_multi_immediate(card, handles, count);
}
int skiq_read_rx_streaming_handles(std::uint8_t, skiq_rx_hdl_t *handles, std::uint8_t *count)
{
    ++sdk.queries;
    std::copy(sdk.streaming.begin(), sdk.streaming.end(), handles);
    *count = static_cast<std::uint8_t>(sdk.streaming.size());
    return sdk.read_status;
}
namespace soapy_sidekiq
{
#include "RxBackendControlMethods.inc"
RxReceiveResult SidekiqRxBackend::receive()
{
    return {};
}
} // namespace soapy_sidekiq
using soapy_sidekiq::SidekiqRxBackend;

TEST_CASE("SDK multi RX start preserves order and requests timestamp synchronization")
{
    sdk = {};
    SidekiqRxBackend backend(0);
    REQUIRE_EQ(backend.startMulti({2, 0}, false), 0);
    REQUIRE((sdk.started == std::vector<skiq_rx_hdl_t>{skiq_rx_hdl_B1, skiq_rx_hdl_A1}));
    REQUIRE(sdk.synced);
    sdk = {};
    REQUIRE_EQ(backend.startMulti({0, 2}, true), 0);
    REQUIRE(sdk.pps);
    REQUIRE(!sdk.synced);
}
TEST_CASE("SDK multi RX stop retries only selected handles still streaming")
{
    sdk = {};
    SidekiqRxBackend backend(0);
    sdk.streaming = {skiq_rx_hdl_A2, skiq_rx_hdl_B1};
    REQUIRE_EQ(backend.stopMulti({0, 2}, false), 0);
    REQUIRE((sdk.stopped == std::vector<skiq_rx_hdl_t>{skiq_rx_hdl_B1}));
    REQUIRE_EQ(sdk.queries, 1u);
}
TEST_CASE("SDK multi RX stop preserves order and PPS for remaining handles")
{
    sdk = {};
    SidekiqRxBackend backend(0);
    sdk.streaming = {skiq_rx_hdl_A1, skiq_rx_hdl_B1};
    REQUIRE_EQ(backend.stopMulti({2, 0}, true), 0);
    REQUIRE((sdk.stopped == std::vector<skiq_rx_hdl_t>{skiq_rx_hdl_B1, skiq_rx_hdl_A1}));
    REQUIRE(sdk.pps);
}
TEST_CASE("SDK multi RX stop distinguishes a batch missing-handle error from full release")
{
    sdk = {};
    SidekiqRxBackend backend(0);
    sdk.streaming = {skiq_rx_hdl_A1, skiq_rx_hdl_B1};
    sdk.stop_status = -ENODEV;
    REQUIRE_EQ(backend.stopMulti({0, 2}, false), -EIO);
    sdk.streaming = {skiq_rx_hdl_B1};
    sdk.stop_status = 0;
    REQUIRE_EQ(backend.stopMulti({0, 2}, false), 0);
    REQUIRE((sdk.stopped == std::vector<skiq_rx_hdl_t>{skiq_rx_hdl_B1}));
    sdk.streaming.clear();
    sdk.stopped.clear();
    REQUIRE_EQ(backend.stopMulti({0, 2}, false), 0);
    REQUIRE(sdk.stopped.empty());
}
TEST_CASE("SDK multi RX stop retains query failures and recognizes a disabled card")
{
    sdk = {};
    SidekiqRxBackend backend(0);
    sdk.read_status = -EIO;
    REQUIRE_EQ(backend.stopMulti({0, 2}, false), -EIO);
    REQUIRE(sdk.stopped.empty());
    sdk.read_status = -ENODEV;
    REQUIRE_EQ(backend.stopMulti({0, 2}, false), -ENODEV);
}
TEST_CASE("SDK RX adapter retains single-handle behavior and rejects empty batches")
{
    sdk = {};
    SidekiqRxBackend backend(0);
    REQUIRE_EQ(backend.startMulti({}, false), -EINVAL);
    REQUIRE_EQ(backend.stopMulti({}, false), -EINVAL);
    REQUIRE_EQ(backend.startMulti({2}, false), 0);
    REQUIRE_EQ(backend.stopMulti({2}, false), 0);
    REQUIRE_EQ(sdk.queries, 0u);
}
