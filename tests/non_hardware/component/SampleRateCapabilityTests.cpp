/**
 * @file SampleRateCapabilityTests.cpp
 * @brief Exercises production sample-rate bounds and built-in profile suggestions.
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
enum skiq_part_t
{
    skiq_m2,
    skiq_nv100,
    skiq_nvm2
};
namespace SoapySDR
{
struct Range
{
    double first, last;
    Range(double minimum, double maximum) : first(minimum), last(maximum) {}
    double minimum() const
    {
        return first;
    }
    double maximum() const
    {
        return last;
    }
};
using RangeList = std::vector<Range>;
} // namespace SoapySDR
namespace
{
struct FakeSdk
{
    std::uint32_t minimum{1000000}, maximum{1800000};
    int minimum_status{}, maximum_status{};
    unsigned minimum_calls{}, maximum_calls{};
} sdk;
} // namespace
int skiq_read_min_sample_rate(std::uint8_t, std::uint32_t *minimum)
{
    ++sdk.minimum_calls;
    *minimum = sdk.minimum;
    return sdk.minimum_status;
}
int skiq_read_max_sample_rate(std::uint8_t, std::uint32_t *maximum)
{
    ++sdk.maximum_calls;
    *maximum = sdk.maximum;
    return sdk.maximum_status;
}
/** The fixture uses real channel validation before the injected SDK calls. */
class SoapySidekiq
{
  public:
    std::uint8_t card{5}, num_rx_channels{1}, num_tx_channels{1};
    skiq_part_t part{skiq_m2};
    struct RateLimits
    {
        std::uint32_t sample_rate_min{1},
            sample_rate_max{std::numeric_limits<std::uint32_t>::max()};
    };
    struct Parameters
    {
        std::array<RateLimits, 2> rx_param{}, tx_param{};
    } param;
    SoapySidekiq()
    {
        sdk = {};
    }
    std::size_t getNumChannels(int) const;
    void validateChannel(int, std::size_t) const;
    SoapySDR::RangeList getSampleRateRange(int, std::size_t) const;
    std::vector<double> listSampleRates(int, std::size_t) const;
};
#include "ChannelValidationMethods.inc"
#include "SampleRateCapabilityMethods.inc"

TEST_CASE("sample-rate capabilities preserve SDK bounds and the legacy 250 kHz grid")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        const auto bounds = device.getSampleRateRange(direction, 0);
        REQUIRE_EQ(bounds.size(), std::size_t{1});
        REQUIRE_EQ(bounds[0].minimum(), 1000000.0);
        REQUIRE_EQ(bounds[0].maximum(), 1800000.0);
        const std::vector<double> expected{1000000, 1250000, 1500000, 1750000};
        REQUIRE(device.listSampleRates(direction, 0) == expected);
    }
}
TEST_CASE("sample-rate capability errors stop at the failing SDK call")
{
    for (const bool fail_minimum : {false, true})
        for (const bool listing : {false, true})
        {
            SoapySidekiq device;
            sdk.minimum_status = fail_minimum ? -EIO : 0;
            sdk.maximum_status = fail_minimum ? 0 : -EIO;
            if (listing)
                REQUIRE_THROWS_AS(device.listSampleRates(SOAPY_SDR_RX, 0), std::runtime_error);
            else
                REQUIRE_THROWS_AS(device.getSampleRateRange(SOAPY_SDR_RX, 0), std::runtime_error);
            REQUIRE_EQ(sdk.minimum_calls, 1u);
            REQUIRE_EQ(sdk.maximum_calls, fail_minimum ? 0u : 1u);
        }
}
TEST_CASE("sample-rate capability queries reject zero and inverted bounds")
{
    for (const bool inverted : {false, true})
    {
        SoapySidekiq device;
        sdk.minimum = inverted ? sdk.maximum + 1 : 0;
        REQUIRE_THROWS_AS(device.getSampleRateRange(SOAPY_SDR_RX, 0), std::runtime_error);
        REQUIRE_THROWS_AS(device.listSampleRates(SOAPY_SDR_TX, 0), std::runtime_error);
    }
}
TEST_CASE("sample-rate list iteration cannot wrap at the uint32 upper boundary")
{
    SoapySidekiq device;
    sdk.maximum = std::numeric_limits<std::uint32_t>::max();
    sdk.minimum = sdk.maximum - 250000;
    const auto rates = device.listSampleRates(SOAPY_SDR_RX, 0);
    REQUIRE_EQ(rates.size(), std::size_t{2});
    REQUIRE_EQ(rates[0], static_cast<double>(sdk.minimum));
    REQUIRE_EQ(rates[1], static_cast<double>(sdk.maximum));
}
TEST_CASE("an equal nonzero sample-rate bound produces one compatibility entry")
{
    SoapySidekiq device;
    sdk.minimum = sdk.maximum;
    const auto rates = device.listSampleRates(SOAPY_SDR_TX, 0);
    REQUIRE_EQ(rates.size(), std::size_t{1});
    REQUIRE_EQ(rates[0], static_cast<double>(sdk.minimum));
}
TEST_CASE("sample-rate capability queries validate directions and channels before SDK calls")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.getSampleRateRange(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.listSampleRates(9, 0), std::invalid_argument);
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        REQUIRE_THROWS_AS(device.getSampleRateRange(direction, 1), std::out_of_range);
        REQUIRE_THROWS_AS(device.listSampleRates(direction, 1), std::out_of_range);
    }
    REQUIRE_EQ(sdk.minimum_calls, 0u);
    REQUIRE_EQ(sdk.maximum_calls, 0u);
}

TEST_CASE("NV100 and NVM2 suggest documented profiles rather than an offset 250 kHz grid")
{
    for (const auto part : {skiq_nv100, skiq_nvm2})
        for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        {
            SoapySidekiq device;
            device.part = part;
            sdk.minimum = 4900000;
            sdk.maximum = 10000000;
            const std::vector<double> expected{4915200, 5000000, 5600000,
                                               7680000, 9830400, 10000000};
            REQUIRE(device.listSampleRates(direction, 0) == expected);
        }
}
TEST_CASE("SDK 4.26 NV100 low-rate profiles are filtered by reported hardware bounds")
{
    SoapySidekiq device;
    device.part = skiq_nv100;
    sdk.minimum = 541667;
    sdk.maximum = 1000000;
    const std::vector<double> expected{541667, 625000, 640000, 740740, 750000, 1000000};
    REQUIRE(device.listSampleRates(SOAPY_SDR_RX, 0) == expected);
    sdk.minimum = 625001;
    sdk.maximum = 639999;
    REQUIRE(device.listSampleRates(SOAPY_SDR_TX, 0).empty());
}
TEST_CASE("profile suggestions respect each selected channel and direction's SDK limits")
{
    SoapySidekiq device;
    device.part = skiq_nv100;
    device.num_rx_channels = device.num_tx_channels = 2;
    sdk.minimum = 1000000;
    sdk.maximum = 10000000;
    device.param.rx_param[1] = {4900000, 5600000};
    device.param.tx_param[1] = {7500000, 10000000};
    REQUIRE(device.listSampleRates(SOAPY_SDR_RX, 1) ==
            std::vector<double>({4915200, 5000000, 5600000}));
    REQUIRE(device.listSampleRates(SOAPY_SDR_TX, 1) ==
            std::vector<double>({7680000, 9830400, 10000000}));
    REQUIRE(device.listSampleRates(SOAPY_SDR_RX, 0).size() > 3);
}
TEST_CASE("invalid per-channel profile metadata does not invent selectable rates")
{
    for (const bool inverted : {false, true})
    {
        SoapySidekiq device;
        device.part = skiq_nv100;
        device.param.rx_param[0] = {inverted ? 5000000u : 0u, 1000000};
        REQUIRE_THROWS_AS(device.listSampleRates(SOAPY_SDR_RX, 0), std::runtime_error);
    }
}
