/**
 * @file FrequencyTests.cpp
 * @brief Exercises production RF frequency methods with an injected SDK.
 */

#include "TestHarness.hpp"
#include <SoapySidekiq/NumericConversion.hpp>
#include <array>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
#define SoapySDR_logf(...) ((void)0)
#define SoapySDR_log(...) ((void)0)
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
namespace SoapySDR
{
using Kwargs = std::map<std::string, std::string>;
struct Range
{
    double minimum, maximum;
    Range(double first, double last) : minimum(first), maximum(last) {}
};
using RangeList = std::vector<Range>;
} // namespace SoapySDR

namespace
{
struct FakeSdk
{
    unsigned calls{}, handle{};
    std::uint8_t card{};
    int direction{}, status{};
    std::uint64_t written{123}, requested{1000000000};
    double actual{1000000000.25};
    std::uint64_t minimum{70000000}, maximum{6000000000};
    bool omit_actual{};
} sdk;

int writeFrequency(int direction, std::uint8_t card, unsigned handle, std::uint64_t value)
{
    ++sdk.calls;
    sdk.direction = direction;
    sdk.card = card;
    sdk.handle = handle;
    if (sdk.status == 0)
        sdk.written = value;
    return sdk.status;
}
int readFrequency(int direction, std::uint8_t card, unsigned handle, std::uint64_t *requested,
                  double *actual)
{
    ++sdk.calls;
    sdk.direction = direction;
    sdk.card = card;
    sdk.handle = handle;
    if (sdk.status == 0)
    {
        *requested = sdk.requested;
        if (!sdk.omit_actual)
            *actual = sdk.actual;
    }
    return sdk.status;
}
int readRange(int direction, std::uint8_t card, std::uint64_t *maximum, std::uint64_t *minimum)
{
    ++sdk.calls;
    sdk.direction = direction;
    sdk.card = card;
    if (sdk.status == 0)
    {
        *maximum = sdk.maximum;
        *minimum = sdk.minimum;
    }
    return sdk.status;
}
} // namespace

int skiq_write_rx_LO_freq(std::uint8_t card, unsigned handle, std::uint64_t value)
{
    return writeFrequency(SOAPY_SDR_RX, card, handle, value);
}
int skiq_write_tx_LO_freq(std::uint8_t card, unsigned handle, std::uint64_t value)
{
    return writeFrequency(SOAPY_SDR_TX, card, handle, value);
}
int skiq_read_rx_LO_freq(std::uint8_t card, unsigned handle, std::uint64_t *value, double *actual)
{
    return readFrequency(SOAPY_SDR_RX, card, handle, value, actual);
}
int skiq_read_tx_LO_freq(std::uint8_t card, unsigned handle, std::uint64_t *value, double *actual)
{
    return readFrequency(SOAPY_SDR_TX, card, handle, value, actual);
}
int skiq_read_rx_LO_freq_range(std::uint8_t card, std::uint64_t *maximum, std::uint64_t *minimum)
{
    return readRange(SOAPY_SDR_RX, card, maximum, minimum);
}
int skiq_read_tx_LO_freq_range(std::uint8_t card, std::uint64_t *maximum, std::uint64_t *minimum)
{
    return readRange(SOAPY_SDR_TX, card, maximum, minimum);
}

/** Device state used by the real validation, mapping, and frequency methods. */
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
    void setFrequency(int, std::size_t, double, const SoapySDR::Kwargs &);
    double getFrequency(int, std::size_t) const;
    SoapySDR::RangeList getFrequencyRange(int, std::size_t) const;
    std::vector<std::string> listFrequencies(int, std::size_t) const;
    void validateFrequencyName(int, std::size_t, const std::string &) const;
    void setFrequency(int, std::size_t, const std::string &, double, const SoapySDR::Kwargs &);
    double getFrequency(int, std::size_t, const std::string &) const;
    SoapySDR::RangeList getFrequencyRange(int, std::size_t, const std::string &) const;
};
#include "ChannelValidationMethods.inc"
#include "ChannelMappingMethods.inc"
#include "FrequencyMethods.inc"

TEST_CASE("RX and TX tuning use the mapped handle and preserve fractional-unit truncation")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        device.setFrequency(direction, 1, 915000000.75, {});
        REQUIRE_EQ(sdk.written, std::uint64_t{915000000});
        REQUIRE_EQ(sdk.handle, direction == SOAPY_SDR_RX ? 7u : 2u);
        REQUIRE_EQ(sdk.card, device.card);
        REQUIRE_EQ(sdk.direction, direction);
    }
}

TEST_CASE("every available RX and TX channel advertises the RF tuning component")
{
    SoapySidekiq device;
    const std::vector<std::string> expected{"RF"};
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (std::size_t channel = 0; channel < device.getNumChannels(direction); ++channel)
            REQUIRE(device.listFrequencies(direction, channel) == expected);
    REQUIRE_EQ(sdk.calls, 0u);
}

TEST_CASE("named RF tuning preserves aggregate conversion readback and mapped handles")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const std::string name : {"RF", "rf", "Rf", "rF"})
        {
            device.setFrequency(direction, 1, name, 915000000.75, {});
            REQUIRE_EQ(sdk.written, std::uint64_t{915000000});
            REQUIRE_EQ(sdk.handle, direction == SOAPY_SDR_RX ? 7u : 2u);
            REQUIRE_EQ(device.getFrequency(direction, 1, name), sdk.actual);
            const auto ranges = device.getFrequencyRange(direction, 1, name);
            REQUIRE_EQ(ranges.size(), std::size_t{1});
            REQUIRE_EQ(ranges[0].minimum, static_cast<double>(sdk.minimum));
            REQUIRE_EQ(ranges[0].maximum, static_cast<double>(sdk.maximum));
        }
}

TEST_CASE("unknown tuning components cannot silently tune the aggregate RF frequency")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const std::string name : {"", "BB", "RF2", "unknown"})
        {
            REQUIRE_THROWS_AS(device.setFrequency(direction, 0, name, 915000000, {}),
                              std::invalid_argument);
            REQUIRE_THROWS_AS(device.getFrequency(direction, 0, name), std::invalid_argument);
            REQUIRE_THROWS_AS(device.getFrequencyRange(direction, 0, name), std::invalid_argument);
        }
    REQUIRE_EQ(sdk.calls, 0u);
}

TEST_CASE("RF component APIs validate directions and channels before SDK access")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.listFrequencies(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.setFrequency(9, 0, "RF", 1, {}), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getFrequency(9, 0, "RF"), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getFrequencyRange(9, 0, "RF"), std::invalid_argument);
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        REQUIRE_THROWS_AS(device.listFrequencies(direction, 2), std::out_of_range);
        REQUIRE_THROWS_AS(device.setFrequency(direction, 2, "RF", 1, {}), std::out_of_range);
        REQUIRE_THROWS_AS(device.getFrequency(direction, 2, "RF"), std::out_of_range);
        REQUIRE_THROWS_AS(device.getFrequencyRange(direction, 2, "RF"), std::out_of_range);
    }
    REQUIRE_EQ(sdk.calls, 0u);
}
TEST_CASE("invalid RF frequencies fail before any SDK call")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const double value :
             {-1.0, std::numeric_limits<double>::quiet_NaN(),
              std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
              std::ldexp(1.0, 64), std::numeric_limits<double>::max()})
            REQUIRE_THROWS_AS(device.setFrequency(direction, 0, value, {}), std::invalid_argument);
    REQUIRE_EQ(sdk.calls, 0u);
    REQUIRE_EQ(sdk.written, std::uint64_t{123});
}
TEST_CASE("RF integer conversion admits the largest representable double below uint64 overflow")
{
    SoapySidekiq device;
    const double below = std::nextafter(std::ldexp(1.0, 64), 0.0);
    device.setFrequency(SOAPY_SDR_TX, 0, below, {});
    REQUIRE_EQ(sdk.written, std::uint64_t{0xfffffffffffff800ULL});
    // Hardware range rejection remains the SDK's responsibility.
    device.setFrequency(SOAPY_SDR_RX, 0, 0, {});
    REQUIRE_EQ(sdk.written, std::uint64_t{0});
}
TEST_CASE("frequency readback returns actual tuned frequency on the selected handle")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        REQUIRE_EQ(device.getFrequency(direction, 1), sdk.actual);
        REQUIRE_EQ(sdk.handle, direction == SOAPY_SDR_RX ? 7u : 2u);
        REQUIRE_EQ(sdk.card, device.card);
        REQUIRE_EQ(sdk.direction, direction);
    }
}
TEST_CASE("frequency operations reject invalid directions and channels before the SDK")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.setFrequency(9, 0, 100, {}), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getFrequency(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getFrequencyRange(9, 0), std::invalid_argument);
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        REQUIRE_THROWS_AS(device.setFrequency(direction, 2, 100, {}), std::out_of_range);
        REQUIRE_THROWS_AS(device.getFrequency(direction, 2), std::out_of_range);
        REQUIRE_THROWS_AS(device.getFrequencyRange(direction, 2), std::out_of_range);
    }
    REQUIRE_EQ(sdk.calls, 0u);
}
TEST_CASE("frequency SDK failures preserve prior tuning and expose useful status context")
{
    SoapySidekiq device;
    sdk.status = -EIO;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        REQUIRE_THROWS_AS(device.setFrequency(direction, 1, 915000000, {}), std::runtime_error);
        try
        {
            device.getFrequency(direction, 1);
            REQUIRE(false);
        }
        catch (const std::runtime_error &error)
        {
            const std::string text(error.what());
            REQUIRE(text.find("LO_freq") != std::string::npos);
            REQUIRE(text.find("card 5") != std::string::npos);
            REQUIRE(text.find("channel 1") != std::string::npos);
            REQUIRE(text.find(std::to_string(-EIO)) != std::string::npos);
        }
        REQUIRE_THROWS_AS(device.getFrequencyRange(direction, 1), std::runtime_error);
    }
    REQUIRE_EQ(sdk.written, std::uint64_t{123});
}
TEST_CASE("frequency readback rejects invalid actual values and missing SDK output")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        for (const double value : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(), std::ldexp(1.0, 64)})
        {
            sdk.actual = value;
            REQUIRE_THROWS_AS(device.getFrequency(direction, 0), std::runtime_error);
        }
        sdk.omit_actual = true;
        REQUIRE_THROWS_AS(device.getFrequency(direction, 0), std::runtime_error);
        sdk.omit_actual = false;
    }
}
TEST_CASE("frequency ranges preserve SDK min/max order and reject inverted bounds")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        sdk.minimum = 70000000;
        sdk.maximum = 6000000000;
        const auto range = device.getFrequencyRange(direction, 1);
        REQUIRE_EQ(range.size(), std::size_t{1});
        REQUIRE_EQ(range[0].minimum, 70000000.0);
        REQUIRE_EQ(range[0].maximum, 6000000000.0);
        sdk.minimum = sdk.maximum + 1;
        REQUIRE_THROWS_AS(device.getFrequencyRange(direction, 1), std::runtime_error);
    }
}
