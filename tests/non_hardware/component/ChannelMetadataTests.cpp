/**
 * @file ChannelMetadataTests.cpp
 * @brief Exercises production channel mapping and metadata without an SDK.
 */

#include "TestHarness.hpp"
#include <SoapySidekiq/StreamConfiguration.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
#define SOAPY_SDR_CS16 "CS16"
#define SOAPY_SDR_CF32 "CF32"
#define SoapySDR_logf(...) ((void)0)
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
enum : unsigned
{
    skiq_rx_hdl_A1,
    skiq_rx_hdl_A2,
    skiq_rx_hdl_B1,
    skiq_rx_hdl_B2,
    skiq_rx_hdl_C1,
    skiq_rx_hdl_D1
};
enum : unsigned
{
    skiq_tx_hdl_A1,
    skiq_tx_hdl_A2,
    skiq_tx_hdl_B1,
    skiq_tx_hdl_B2
};

namespace SoapySDR
{
using Kwargs = std::map<std::string, std::string>;
struct ArgInfo
{
    std::string key, name, description, units, value;
    enum Type
    {
        INT
    } type{};
};
using ArgInfoList = std::vector<ArgInfo>;
} // namespace SoapySDR

/** Only SDK channel tables and metadata state are needed by these real methods. */
class SoapySidekiq
{
  public:
    struct HandleEntry
    {
        unsigned handle;
    };
    struct Parameters
    {
        std::array<HandleEntry, 2> rx_param{{{3}, {7}}};
        std::array<HandleEntry, 3> tx_param{{{6}, {2}, {9}}};
    } param;
    std::uint8_t num_rx_channels{2}, num_tx_channels{3};
    std::uint8_t card{5};
    std::string part_str{"test-card"};
    double maxValue{2047}, txMaxValue{8191};
    std::uint32_t rx_payload_size_in_words{1020}, current_tx_block_size{252};

    std::size_t getNumChannels(int) const;
    SoapySDR::Kwargs getChannelInfo(int, std::size_t) const;
    void validateChannel(int, std::size_t) const;
    skiq_rx_hdl_t getRxHandle(std::size_t) const;
    skiq_tx_hdl_t getTxHandle(std::size_t) const;
    std::vector<std::string> getStreamFormats(int, std::size_t) const;
    std::string getNativeStreamFormat(int, std::size_t, double &) const;
    SoapySDR::ArgInfoList getStreamArgsInfo(int, std::size_t) const;
};

static soapy_sidekiq::StreamDirection streamDirection(int direction)
{
    if (direction == SOAPY_SDR_RX)
        return soapy_sidekiq::StreamDirection::rx;
    if (direction == SOAPY_SDR_TX)
        return soapy_sidekiq::StreamDirection::tx;
    throw std::invalid_argument("invalid stream direction");
}

#include "ChannelValidationMethods.inc"
#include "ChannelMetadataMethods.inc"

TEST_CASE("logical channels map to the SDK table even for non-contiguous handles")
{
    SoapySidekiq device;
    REQUIRE_EQ(device.getNumChannels(SOAPY_SDR_RX), std::size_t{2});
    REQUIRE_EQ(device.getNumChannels(SOAPY_SDR_TX), std::size_t{3});
    REQUIRE_EQ(device.getRxHandle(0), 3u);
    REQUIRE_EQ(device.getRxHandle(1), 7u);
    REQUIRE_EQ(device.getTxHandle(0), 6u);
    REQUIRE_EQ(device.getTxHandle(1), 2u);
    REQUIRE_EQ(device.getTxHandle(2), 9u);
}

TEST_CASE("channel mapping rejects count boundaries and oversized indices")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.getRxHandle(2), std::out_of_range);
    REQUIRE_THROWS_AS(device.getTxHandle(3), std::out_of_range);
    REQUIRE_THROWS_AS(device.getRxHandle(std::numeric_limits<std::size_t>::max()),
                      std::out_of_range);
    REQUIRE_THROWS_AS(device.getTxHandle(std::numeric_limits<std::size_t>::max()),
                      std::out_of_range);
    device.num_rx_channels = 0;
    device.num_tx_channels = 0;
    REQUIRE_THROWS_AS(device.getRxHandle(0), std::out_of_range);
    REQUIRE_THROWS_AS(device.getTxHandle(0), std::out_of_range);
}

TEST_CASE("channel labels identify mapped SDK handles rather than logical channel numbers")
{
    SoapySidekiq device;
    device.param.rx_param[0].handle = skiq_rx_hdl_B1;
    device.param.rx_param[1].handle = skiq_rx_hdl_A1;
    device.param.tx_param[0].handle = skiq_tx_hdl_A2;
    REQUIRE_EQ(device.getChannelInfo(SOAPY_SDR_RX, 0).at("label"), std::string{"RX B1"});
    REQUIRE_EQ(device.getChannelInfo(SOAPY_SDR_RX, 1).at("label"), std::string{"RX A1"});
    REQUIRE_EQ(device.getChannelInfo(SOAPY_SDR_TX, 0).at("label"), std::string{"TX A2"});
    const auto info = device.getChannelInfo(SOAPY_SDR_RX, 0);
    REQUIRE_EQ(info.at("card"), std::string{"5"});
    REQUIRE_EQ(info.at("card_type"), std::string{"test-card"});
    REQUIRE_EQ(info.at("sidekiq_channel"), std::string{"0"});
    REQUIRE_EQ(info.at("sidekiq_handle"), std::string{"B1"});
    REQUIRE_EQ(info.at("sidekiq_handle_id"), std::to_string(skiq_rx_hdl_B1));
}

TEST_CASE("unknown SDK handle values retain a useful numeric channel label")
{
    SoapySidekiq device;
    REQUIRE_EQ(device.getChannelInfo(SOAPY_SDR_RX, 1).at("label"), std::string{"RX 7"});
    REQUIRE_EQ(device.getChannelInfo(SOAPY_SDR_TX, 2).at("sidekiq_handle_id"), std::string{"9"});
}

TEST_CASE("channel information rejects invalid directions and unavailable channels")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.getChannelInfo(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getChannelInfo(SOAPY_SDR_RX, 2), std::out_of_range);
    REQUIRE_THROWS_AS(device.getChannelInfo(SOAPY_SDR_TX, 3), std::out_of_range);
    REQUIRE_THROWS_AS(device.getChannelInfo(SOAPY_SDR_RX, std::numeric_limits<std::size_t>::max()),
                      std::out_of_range);
}

TEST_CASE("every exposed channel advertises both implemented stream formats")
{
    SoapySidekiq device;
    const std::vector<std::string> expected{"CS16", "CF32"};
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (std::size_t channel = 0; channel < device.getNumChannels(direction); ++channel)
            REQUIRE(device.getStreamFormats(direction, channel) == expected);
}

TEST_CASE("stream metadata uses direction-specific full scale and block size")
{
    SoapySidekiq device;
    double scale = 0;
    REQUIRE_EQ(device.getNativeStreamFormat(SOAPY_SDR_RX, 1, scale), std::string{"CS16"});
    REQUIRE_EQ(scale, 2047.0);
    REQUIRE_EQ(device.getNativeStreamFormat(SOAPY_SDR_TX, 2, scale), std::string{"CS16"});
    REQUIRE_EQ(scale, 8191.0);
    const auto rx = device.getStreamArgsInfo(SOAPY_SDR_RX, 1);
    const auto tx = device.getStreamArgsInfo(SOAPY_SDR_TX, 2);
    REQUIRE_EQ(rx.size(), std::size_t{1});
    REQUIRE_EQ(tx.size(), std::size_t{1});
    REQUIRE_EQ(rx[0].key, std::string{"bufflen"});
    REQUIRE_EQ(tx[0].key, std::string{"bufflen"});
    REQUIRE_EQ(rx[0].value, std::string{"1020"});
    REQUIRE_EQ(tx[0].value, std::string{"252"});
}

TEST_CASE("metadata consistently rejects invalid directions without changing output")
{
    SoapySidekiq device;
    for (const int direction : {-1, 2, std::numeric_limits<int>::max()})
    {
        double scale = 123;
        REQUIRE_THROWS_AS(device.getNumChannels(direction), std::invalid_argument);
        REQUIRE_THROWS_AS(device.getStreamFormats(direction, 0), std::invalid_argument);
        REQUIRE_THROWS_AS(device.getNativeStreamFormat(direction, 0, scale), std::invalid_argument);
        REQUIRE_THROWS_AS(device.getStreamArgsInfo(direction, 0), std::invalid_argument);
        REQUIRE_EQ(scale, 123.0);
    }
}

TEST_CASE("metadata consistently rejects unavailable channels without changing output")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        double scale = 456;
        const auto channel = device.getNumChannels(direction);
        REQUIRE_THROWS_AS(device.getStreamFormats(direction, channel), std::out_of_range);
        REQUIRE_THROWS_AS(device.getNativeStreamFormat(direction, channel, scale),
                          std::out_of_range);
        REQUIRE_THROWS_AS(device.getStreamArgsInfo(direction, channel), std::out_of_range);
        REQUIRE_EQ(scale, 456.0);
    }
}
