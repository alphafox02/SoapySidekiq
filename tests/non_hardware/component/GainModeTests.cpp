/**
 * @file GainModeTests.cpp
 * @brief Exercises production gain-mode capabilities and SDK error handling.
 */
#include "TestHarness.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
#define SoapySDR_logf(...) ((void)0)
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
enum skiq_rx_gain_t
{
    skiq_rx_gain_manual,
    skiq_rx_gain_auto,
    unsupported_mode
};
namespace
{
struct FakeSdk
{
    std::array<skiq_rx_gain_t, 10> modes{};
    unsigned reads{}, writes{}, handle{};
    std::uint8_t card{};
    int read_status{}, write_status{};
} sdk;
} // namespace
int skiq_read_rx_gain_mode(std::uint8_t card, unsigned handle, skiq_rx_gain_t *mode)
{
    ++sdk.reads;
    sdk.card = card;
    sdk.handle = handle;
    if (sdk.read_status == 0)
        *mode = sdk.modes.at(handle);
    return sdk.read_status;
}
int skiq_write_rx_gain_mode(std::uint8_t card, unsigned handle, skiq_rx_gain_t mode)
{
    ++sdk.writes;
    sdk.card = card;
    sdk.handle = handle;
    if (sdk.write_status == 0)
        sdk.modes.at(handle) = mode;
    return sdk.write_status;
}
/** Metadata and SDK table stand-ins for the extracted production methods. */
class SoapySidekiq
{
  public:
    struct Entry
    {
        unsigned handle;
    };
    struct Parameters
    {
        std::array<Entry, 2> rx_param{{{3}, {7}}};
        std::array<Entry, 2> tx_param{{{6}, {2}}};
    } param;
    std::uint8_t card{5}, num_rx_channels{2}, num_tx_channels{2};
    SoapySidekiq()
    {
        sdk = {};
    }
    std::size_t getNumChannels(int) const;
    void validateChannel(int, std::size_t) const;
    skiq_rx_hdl_t getRxHandle(std::size_t) const;
    skiq_tx_hdl_t getTxHandle(std::size_t) const;
    std::vector<std::string> listGains(int, std::size_t) const;
    bool hasGainMode(int, std::size_t) const;
    void setGainMode(int, std::size_t, bool);
    bool getGainMode(int, std::size_t) const;
};
#include "ChannelValidationMethods.inc"
#include "ChannelMappingMethods.inc"
#include "GainModeMethods.inc"

TEST_CASE("gain-mode capability reports RX control and manual-only TX without SDK calls")
{
    SoapySidekiq device;
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        REQUIRE(device.hasGainMode(SOAPY_SDR_RX, channel));
        REQUIRE(!device.hasGainMode(SOAPY_SDR_TX, channel));
        const std::vector<std::string> expected{"LNA"};
        REQUIRE(device.listGains(SOAPY_SDR_RX, channel) == expected);
        const std::vector<std::string> tx_expected{"LNA", "attenuation"};
        REQUIRE(device.listGains(SOAPY_SDR_TX, channel) == tx_expected);
    }
    REQUIRE_EQ(sdk.reads, 0u);
    REQUIRE_EQ(sdk.writes, 0u);
}
TEST_CASE("RX gain-mode APIs use the supplied channel and actual SDK readback")
{
    SoapySidekiq device;
    device.setGainMode(SOAPY_SDR_RX, 1, true);
    REQUIRE_EQ(sdk.handle, 7u);
    REQUIRE_EQ(sdk.card, device.card);
    REQUIRE(sdk.modes[7] == skiq_rx_gain_auto);
    REQUIRE(sdk.modes[3] == skiq_rx_gain_manual);
    REQUIRE(device.getGainMode(SOAPY_SDR_RX, 1));
    device.setGainMode(SOAPY_SDR_RX, 1, false);
    REQUIRE(!device.getGainMode(SOAPY_SDR_RX, 1));
    sdk.modes[7] = skiq_rx_gain_auto;
    REQUIRE(device.getGainMode(SOAPY_SDR_RX, 1));
    device.setGainMode(SOAPY_SDR_RX, 0, false);
    REQUIRE_EQ(sdk.handle, 3u);
}
TEST_CASE("TX rejects automatic gain selection and accepts manual without SDK calls")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.setGainMode(SOAPY_SDR_TX, 1, true), std::invalid_argument);
    device.setGainMode(SOAPY_SDR_TX, 1, false);
    REQUIRE(!device.getGainMode(SOAPY_SDR_TX, 1));
    REQUIRE_EQ(sdk.reads, 0u);
    REQUIRE_EQ(sdk.writes, 0u);
}
TEST_CASE("gain-mode APIs reject invalid directions before reporting metadata or touching SDK")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.listGains(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.hasGainMode(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getGainMode(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.setGainMode(9, 0, true), std::invalid_argument);
    REQUIRE_EQ(sdk.reads, 0u);
    REQUIRE_EQ(sdk.writes, 0u);
}
TEST_CASE("gain-mode APIs reject unavailable channels including manual-only TX")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const std::size_t channel : {std::size_t{2}, std::numeric_limits<std::size_t>::max()})
        {
            REQUIRE_THROWS_AS(device.listGains(direction, channel), std::out_of_range);
            REQUIRE_THROWS_AS(device.hasGainMode(direction, channel), std::out_of_range);
            REQUIRE_THROWS_AS(device.getGainMode(direction, channel), std::out_of_range);
            REQUIRE_THROWS_AS(device.setGainMode(direction, channel, false), std::out_of_range);
        }
    REQUIRE_EQ(sdk.reads, 0u);
    REQUIRE_EQ(sdk.writes, 0u);
}
TEST_CASE("gain-mode SDK write failure preserves previous mode and includes context")
{
    SoapySidekiq device;
    sdk.modes[7] = skiq_rx_gain_auto;
    sdk.write_status = -EIO;
    bool threw = false;
    try
    {
        device.setGainMode(SOAPY_SDR_RX, 1, false);
    }
    catch (const std::runtime_error &error)
    {
        threw = true;
        const std::string text(error.what());
        REQUIRE(text.find("skiq_write_rx_gain_mode") != std::string::npos);
        REQUIRE(text.find("card 5") != std::string::npos);
        REQUIRE(text.find("channel 1") != std::string::npos);
        REQUIRE(text.find("handle 7") != std::string::npos);
        REQUIRE(text.find(std::to_string(-EIO)) != std::string::npos);
    }
    REQUIRE(threw);
    REQUIRE(sdk.modes[7] == skiq_rx_gain_auto);
}
TEST_CASE("gain-mode SDK read failure raises an error instead of reporting manual mode")
{
    SoapySidekiq device;
    sdk.read_status = -EIO;
    bool threw = false;
    try
    {
        device.getGainMode(SOAPY_SDR_RX, 1);
    }
    catch (const std::runtime_error &error)
    {
        threw = true;
        const std::string text(error.what());
        REQUIRE(text.find("skiq_read_rx_gain_mode") != std::string::npos);
        REQUIRE(text.find("channel 1") != std::string::npos);
        REQUIRE(text.find("handle 7") != std::string::npos);
        REQUIRE(text.find(std::to_string(-EIO)) != std::string::npos);
    }
    REQUIRE(threw);
    REQUIRE_EQ(sdk.reads, 1u);
    REQUIRE_EQ(sdk.writes, 0u);
}

TEST_CASE("gain-mode readback rejects SDK modes outside the implemented manual and auto choices")
{
    SoapySidekiq device;
    sdk.modes[7] = unsupported_mode;
    REQUIRE_THROWS_AS(device.getGainMode(SOAPY_SDR_RX, 1), std::runtime_error);
}
