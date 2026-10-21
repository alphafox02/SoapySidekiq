/**
 * @file GainTests.cpp
 * @brief Exercises production gain conversion with mapped fake SDK handles.
 */
#include "TestHarness.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
#define SoapySDR_logf(...) ((void)0)
#define SoapySDR_log(...) ((void)0)
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
constexpr skiq_rx_hdl_t skiq_rx_hdl_B1 = 2;
enum skiq_part_t
{
    skiq_mpcie,
    skiq_m2,
    skiq_m2_2280,
    skiq_z2,
    skiq_z3u,
    skiq_x2,
    skiq_x4,
    skiq_x40,
    skiq_nv100,
    skiq_nvm2,
    skiq_z4,
    skiq_z4_mp,
    unknown_part
};
enum skiq_rx_gain_t
{
    skiq_rx_gain_manual,
    skiq_rx_gain_auto
};
namespace SoapySDR
{
struct Range
{
    double first, last, increment;
    Range(double minimum, double maximum, double step)
        : first(minimum), last(maximum), increment(step)
    {
    }
    double minimum() const
    {
        return first;
    }
    double maximum() const
    {
        return last;
    }
    double step() const
    {
        return increment;
    }
};
} // namespace SoapySDR
namespace
{
struct State
{
    std::uint8_t minimum{}, maximum{76}, gain{};
    std::uint16_t attenuation{};
    skiq_rx_gain_t mode{skiq_rx_gain_manual};
};
struct FakeSdk
{
    std::array<State, 10> state;
    std::string events;
    char fail_on{};
    unsigned handle{};
    std::uint8_t card{};
    int touch(char event, std::uint8_t selected_card, unsigned selected_handle)
    {
        events += event;
        card = selected_card;
        handle = selected_handle;
        return fail_on == event ? -EIO : 0;
    }
} sdk;
struct ProfileCase
{
    skiq_part_t part;
    unsigned offset, steps, maximum, tx_maximum;
};
const std::array<ProfileCase, 12> profiles{{{skiq_mpcie, 0, 1, 76, 359},
                                            {skiq_m2, 0, 1, 76, 359},
                                            {skiq_m2_2280, 0, 1, 76, 359},
                                            {skiq_z2, 0, 1, 76, 359},
                                            {skiq_z3u, 0, 1, 76, 359},
                                            {skiq_x2, 195, 2, 30, 167},
                                            {skiq_x4, 195, 2, 30, 167},
                                            {skiq_x40, 195, 2, 30, 167},
                                            {skiq_nv100, 187, 2, 34, 167},
                                            {skiq_nvm2, 187, 2, 34, 167},
                                            {skiq_z4, 187, 2, 34, 167},
                                            {skiq_z4_mp, 187, 2, 34, 167}}};
} // namespace
int skiq_read_rx_gain_index_range(std::uint8_t card, unsigned handle, std::uint8_t *minimum,
                                  std::uint8_t *maximum)
{
    const int status = sdk.touch('R', card, handle);
    *minimum = sdk.state.at(handle).minimum;
    *maximum = sdk.state.at(handle).maximum;
    return status;
}
int skiq_read_rx_gain_mode(std::uint8_t card, unsigned handle, skiq_rx_gain_t *mode)
{
    const int status = sdk.touch('M', card, handle);
    *mode = sdk.state.at(handle).mode;
    return status;
}
int skiq_write_rx_gain_mode(std::uint8_t card, unsigned handle, skiq_rx_gain_t mode)
{
    const int status = sdk.touch('A', card, handle);
    if (status == 0)
        sdk.state.at(handle).mode = mode;
    return status;
}
int skiq_write_rx_gain(std::uint8_t card, unsigned handle, std::uint8_t gain)
{
    const int status = sdk.touch('G', card, handle);
    if (status == 0)
        sdk.state.at(handle).gain = gain;
    return status;
}
int skiq_read_rx_gain(std::uint8_t card, unsigned handle, std::uint8_t *gain)
{
    const int status = sdk.touch('g', card, handle);
    *gain = sdk.state.at(handle).gain;
    return status;
}
int skiq_write_tx_attenuation(std::uint8_t card, unsigned handle, std::uint16_t index)
{
    const int status = sdk.touch('T', card, handle);
    if (status == 0)
        sdk.state.at(handle).attenuation = index;
    return status;
}
int skiq_read_tx_attenuation(std::uint8_t card, unsigned handle, std::uint16_t *index)
{
    const int status = sdk.touch('t', card, handle);
    *index = sdk.state.at(handle).attenuation;
    return status;
}
/** Only SDK tables and card metadata are supplied; methods come from production. */
class SoapySidekiq
{
  public:
    struct RxEntry
    {
        unsigned handle;
    };
    struct TxEntry
    {
        unsigned handle;
        std::uint16_t atten_quarter_db_max{359}, atten_quarter_db_min{};
    };
    struct Parameters
    {
        std::array<RxEntry, 2> rx_param{{{3}, {7}}};
        std::array<TxEntry, 2> tx_param{{{6}, {2}}};
    } param;
    struct GainProfile
    {
        double rx_offset, rx_steps_per_db, rx_maximum;
    };
    skiq_part_t part{skiq_m2};
    std::uint8_t card{5}, num_rx_channels{2}, num_tx_channels{2};
    SoapySidekiq()
    {
        sdk = {};
    }
    void configure(const ProfileCase &profile)
    {
        part = profile.part;
        for (auto &state : sdk.state)
        {
            state.minimum = static_cast<std::uint8_t>(profile.offset);
            state.maximum =
                static_cast<std::uint8_t>(profile.offset + profile.steps * profile.maximum);
        }
        for (auto &entry : param.tx_param)
            entry.atten_quarter_db_max = static_cast<std::uint16_t>(profile.tx_maximum);
    }
    std::size_t getNumChannels(int) const;
    void validateChannel(int, std::size_t) const;
    void validateGainName(int, std::size_t, const std::string &) const;
    std::vector<std::string> listGains(int, std::size_t) const;
    bool hasGainMode(int, std::size_t) const;
    void setGainMode(int, std::size_t, bool);
    bool getGainMode(int, std::size_t) const;
    skiq_rx_hdl_t getRxHandle(std::size_t) const;
    skiq_tx_hdl_t getTxHandle(std::size_t) const;
    GainProfile gainProfile(std::size_t) const;
    double txGainMaximum(std::size_t) const;
    void setGain(int, std::size_t, double);
    void setGain(int, std::size_t, const std::string &, double);
    double getGain(int, std::size_t) const;
    double getGain(int, std::size_t, const std::string &) const;
    SoapySDR::Range getGainRange(int, std::size_t) const;
    SoapySDR::Range getGainRange(int, std::size_t, const std::string &) const;
};
#include "ChannelValidationMethods.inc"
#include "ChannelMappingMethods.inc"
#include "GainModeMethods.inc"
#include "GainMethods.inc"

TEST_CASE("all existing RX profiles retain their index offsets, rounding and advertised ranges")
{
    for (const auto &profile : profiles)
    {
        SoapySidekiq device;
        device.configure(profile);
        device.setGain(SOAPY_SDR_RX, 1, 4.25);
        REQUIRE_EQ(sdk.handle, 7u);
        REQUIRE_EQ(sdk.card, device.card);
        REQUIRE_EQ(sdk.state[7].gain, profile.offset + (profile.steps == 1 ? 4u : 9u));
        REQUIRE_EQ(device.getGain(SOAPY_SDR_RX, 1), profile.steps == 1 ? 4.0 : 4.5);
        const auto range = device.getGainRange(SOAPY_SDR_RX, 1);
        REQUIRE_EQ(range.minimum(), 0.0);
        REQUIRE_EQ(range.maximum(), static_cast<double>(profile.maximum));
        REQUIRE_EQ(range.step(), 1.0 / profile.steps);
    }
}
TEST_CASE("RX clamps large positive and negative requests before any integer narrowing")
{
    for (const auto &profile : profiles)
    {
        SoapySidekiq device;
        device.configure(profile);
        for (const double value : {-std::numeric_limits<double>::max(), -1000.0, -0.75, 1000.0,
                                   std::numeric_limits<double>::max()})
        {
            device.setGain(SOAPY_SDR_RX, 0, value);
            REQUIRE_EQ(sdk.state[3].gain, value < 0 ? sdk.state[3].minimum : sdk.state[3].maximum);
        }
    }
}
TEST_CASE("RX respects narrower and equal hardware index bounds")
{
    SoapySidekiq device;
    device.configure(profiles[5]);
    sdk.state[3].minimum = 200;
    sdk.state[3].maximum = 210;
    device.setGain(SOAPY_SDR_RX, 0, -1000);
    REQUIRE_EQ(sdk.state[3].gain, 200u);
    device.setGain(SOAPY_SDR_RX, 0, 1000);
    REQUIRE_EQ(sdk.state[3].gain, 210u);
    sdk.state[3].maximum = 200;
    device.setGain(SOAPY_SDR_RX, 0, 3);
    REQUIRE_EQ(sdk.state[3].gain, 200u);
}
TEST_CASE("X2 observation gain follows the mapped B1 handle rather than the channel index")
{
    for (const std::size_t channel : {std::size_t{0}, std::size_t{1}})
    {
        SoapySidekiq device;
        device.part = skiq_x2;
        device.param.rx_param[channel].handle = skiq_rx_hdl_B1;
        // Inject an 18-step B1 table; its starting index comes from the SDK.
        sdk.state[skiq_rx_hdl_B1].minimum = 100;
        sdk.state[skiq_rx_hdl_B1].maximum = 118;
        const auto range = device.getGainRange(SOAPY_SDR_RX, channel);
        REQUIRE_EQ(range.minimum(), 0.0);
        REQUIRE_EQ(range.maximum(), 18.0);
        REQUIRE_EQ(range.step(), 1.0);
        for (const double value : {-std::numeric_limits<double>::max(), -1.0, 0.0, 0.49, 0.5, 4.25,
                                   17.6, 18.0, std::numeric_limits<double>::max()})
        {
            device.setGain(SOAPY_SDR_RX, channel, value);
            const auto expected = std::round(std::clamp(value, 0.0, 18.0));
            REQUIRE_EQ(sdk.handle, skiq_rx_hdl_B1);
            REQUIRE_EQ(sdk.state[skiq_rx_hdl_B1].gain, static_cast<std::uint8_t>(100 + expected));
            REQUIRE_EQ(device.getGain(SOAPY_SDR_RX, channel), expected);
        }
    }
}
TEST_CASE("X2 main and observation receivers retain different gain steps on one device")
{
    SoapySidekiq device;
    device.part = skiq_x2;
    device.param.rx_param[0].handle = skiq_rx_hdl_B1;
    device.param.rx_param[1].handle = 0;
    sdk.state[skiq_rx_hdl_B1].minimum = 237;
    sdk.state[skiq_rx_hdl_B1].maximum = 255;
    sdk.state[0].minimum = 195;
    sdk.state[0].maximum = 255;
    device.setGain(SOAPY_SDR_RX, 0, 4.25);
    device.setGain(SOAPY_SDR_RX, 1, 4.25);
    REQUIRE_EQ(sdk.state[skiq_rx_hdl_B1].gain, 241u);
    REQUIRE_EQ(sdk.state[0].gain, 204u);
    REQUIRE_EQ(device.getGain(SOAPY_SDR_RX, 0), 4.0);
    REQUIRE_EQ(device.getGain(SOAPY_SDR_RX, 1), 4.5);
    REQUIRE_EQ(device.getGainRange(SOAPY_SDR_RX, 0).step(), 1.0);
    REQUIRE_EQ(device.getGainRange(SOAPY_SDR_RX, 1).step(), 0.5);
}
TEST_CASE("RX gain conversion and advertised ranges share the SDK index bounds")
{
    for (const auto &profile : profiles)
    {
        SoapySidekiq device;
        device.configure(profile);
        sdk.state[3].minimum = 100;
        sdk.state[3].maximum = 110;
        const auto range = device.getGainRange(SOAPY_SDR_RX, 0);
        REQUIRE_EQ(range.minimum(), 0.0);
        REQUIRE_EQ(range.maximum(), 10.0 / profile.steps);
        REQUIRE_EQ(range.step(), 1.0 / profile.steps);
        device.setGain(SOAPY_SDR_RX, 0, 0);
        REQUIRE_EQ(sdk.state[3].gain, 100u);
        REQUIRE_EQ(device.getGain(SOAPY_SDR_RX, 0), 0.0);
        device.setGain(SOAPY_SDR_RX, 0, range.maximum());
        REQUIRE_EQ(sdk.state[3].gain, 110u);
        REQUIRE_EQ(device.getGain(SOAPY_SDR_RX, 0), range.maximum());
        sdk.state[3].maximum = 100;
        REQUIRE_EQ(device.getGainRange(SOAPY_SDR_RX, 0).maximum(), 0.0);
        device.setGain(SOAPY_SDR_RX, 0, std::numeric_limits<double>::max());
        REQUIRE_EQ(sdk.state[3].gain, 100u);
    }
}
TEST_CASE("explicit RX gain switches automatic mode only after range validation")
{
    SoapySidekiq device;
    sdk.state[7].mode = skiq_rx_gain_auto;
    device.setGain(SOAPY_SDR_RX, 1, 5);
    REQUIRE_EQ(sdk.events, std::string{"RMAG"});
    REQUIRE(sdk.state[7].mode == skiq_rx_gain_manual);
    sdk.events.clear();
    device.setGain(SOAPY_SDR_RX, 1, 6);
    REQUIRE_EQ(sdk.events, std::string{"RMG"});
}
TEST_CASE("failed or inverted RX index ranges do not change automatic mode")
{
    for (const bool sdk_error : {false, true})
    {
        SoapySidekiq device;
        sdk.state[3].mode = skiq_rx_gain_auto;
        if (sdk_error)
            sdk.fail_on = 'R';
        else
        {
            sdk.state[3].minimum = 50;
            sdk.state[3].maximum = 40;
        }
        REQUIRE_THROWS_AS(device.setGain(SOAPY_SDR_RX, 0, 5), std::runtime_error);
        REQUIRE_EQ(sdk.events, std::string{"R"});
        REQUIRE(sdk.state[3].mode == skiq_rx_gain_auto);
    }
}
TEST_CASE("unknown RX gain profiles fail before SDK operations")
{
    SoapySidekiq device;
    device.part = unknown_part;
    REQUIRE_THROWS_AS(device.setGain(SOAPY_SDR_RX, 0, 5), std::runtime_error);
    REQUIRE_THROWS_AS(device.getGain(SOAPY_SDR_RX, 0), std::runtime_error);
    REQUIRE_THROWS_AS(device.getGainRange(SOAPY_SDR_RX, 0), std::runtime_error);
    REQUIRE(sdk.events.empty());
}
TEST_CASE("gain APIs validate directions and channels before touching SDK tables")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.setGain(9, 0, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getGain(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getGainRange(9, 0), std::invalid_argument);
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const std::size_t channel : {std::size_t{2}, std::numeric_limits<std::size_t>::max()})
        {
            REQUIRE_THROWS_AS(device.setGain(direction, channel, 1), std::out_of_range);
            REQUIRE_THROWS_AS(device.getGain(direction, channel), std::out_of_range);
            REQUIRE_THROWS_AS(device.getGainRange(direction, channel), std::out_of_range);
        }
    REQUIRE(sdk.events.empty());
}
TEST_CASE("non-finite gains are rejected before SDK reads or mode changes")
{
    SoapySidekiq device;
    sdk.state[3].mode = skiq_rx_gain_auto;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const double value :
             {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
              -std::numeric_limits<double>::infinity()})
            REQUIRE_THROWS_AS(device.setGain(direction, 0, value), std::invalid_argument);
    REQUIRE(sdk.events.empty());
    REQUIRE(sdk.state[3].mode == skiq_rx_gain_auto);
}
TEST_CASE("TX quarter-dB conversion and readback use SDK limits for every card profile")
{
    for (const auto &profile : profiles)
    {
        SoapySidekiq device;
        device.configure(profile);
        for (const double value : {0.0, 0.125, 0.375, 4.25, profile.tx_maximum / 4.0})
        {
            device.setGain(SOAPY_SDR_TX, 1, value);
            REQUIRE_EQ(sdk.handle, 2u);
            REQUIRE_EQ(sdk.state[2].attenuation,
                       profile.tx_maximum - static_cast<unsigned>(std::round(value * 4)));
            REQUIRE_EQ(device.getGain(SOAPY_SDR_TX, 1), std::round(value * 4) / 4.0);
        }
        const auto range = device.getGainRange(SOAPY_SDR_TX, 1);
        REQUIRE_EQ(range.maximum(), profile.tx_maximum / 4.0);
        REQUIRE_EQ(range.step(), 0.25);
    }
}
TEST_CASE("nonzero TX minimum attenuation limits both conversion and advertised gain")
{
    SoapySidekiq device;
    device.param.tx_param[0].atten_quarter_db_min = 10;
    device.param.tx_param[0].atten_quarter_db_max = 110;
    REQUIRE_EQ(device.getGainRange(SOAPY_SDR_TX, 0).maximum(), 25.0);
    device.setGain(SOAPY_SDR_TX, 0, 25);
    REQUIRE_EQ(sdk.state[6].attenuation, 10u);
    device.setGain(SOAPY_SDR_TX, 0, 0);
    REQUIRE_EQ(sdk.state[6].attenuation, 110u);
    for (const unsigned index : {9u, 111u})
    {
        sdk.state[6].attenuation = static_cast<std::uint16_t>(index);
        REQUIRE_THROWS_AS(device.getGain(SOAPY_SDR_TX, 0), std::runtime_error);
    }
}
TEST_CASE("inverted TX metadata fails before submission or readback")
{
    SoapySidekiq device;
    device.param.tx_param[0].atten_quarter_db_min = 100;
    device.param.tx_param[0].atten_quarter_db_max = 50;
    REQUIRE_THROWS_AS(device.setGain(SOAPY_SDR_TX, 0, 0), std::runtime_error);
    REQUIRE_THROWS_AS(device.getGain(SOAPY_SDR_TX, 0), std::runtime_error);
    REQUIRE_THROWS_AS(device.getGainRange(SOAPY_SDR_TX, 0), std::runtime_error);
    REQUIRE(sdk.events.empty());
}
TEST_CASE("TX rejects out-of-range finite gains without touching hardware")
{
    SoapySidekiq device;
    for (const double value : {-1.0, 90.0, std::numeric_limits<double>::max()})
        REQUIRE_THROWS_AS(device.setGain(SOAPY_SDR_TX, 0, value), std::invalid_argument);
    REQUIRE(sdk.events.empty());
}
TEST_CASE("gain SDK failures stop subsequent writes and report their operation and status")
{
    for (const char failure : {'M', 'A', 'G', 'g', 'T', 't'})
    {
        SoapySidekiq device;
        sdk.fail_on = failure;
        sdk.state[3].mode = skiq_rx_gain_auto;
        bool threw = false;
        try
        {
            if (failure == 'g')
                device.getGain(SOAPY_SDR_RX, 0);
            else if (failure == 't')
                device.getGain(SOAPY_SDR_TX, 0);
            else
                device.setGain(failure == 'T' ? SOAPY_SDR_TX : SOAPY_SDR_RX, 0, 5);
        }
        catch (const std::runtime_error &error)
        {
            threw = true;
            const std::string text(error.what());
            REQUIRE(text.find("skiq_") != std::string::npos);
            REQUIRE(text.find(std::to_string(-EIO)) != std::string::npos);
        }
        REQUIRE(threw);
        REQUIRE_EQ(sdk.events.back(), failure);
        REQUIRE_EQ(sdk.state[3].gain, 0u);
    }
}
TEST_CASE("named gain entry points share checked conversion and range reporting")
{
    SoapySidekiq device;
    device.setGain(SOAPY_SDR_TX, 1, "LNA", 4.25);
    REQUIRE_EQ(device.getGain(SOAPY_SDR_TX, 1, "LNA"), 4.25);
    REQUIRE_EQ(device.getGainRange(SOAPY_SDR_TX, 1, "LNA").maximum(), 89.75);
}
TEST_CASE("TX conversion safely spans the entire SDK uint16 attenuation field")
{
    SoapySidekiq device;
    device.param.tx_param[0].atten_quarter_db_max = std::numeric_limits<std::uint16_t>::max();
    device.setGain(SOAPY_SDR_TX, 0, 16383.75);
    REQUIRE_EQ(sdk.state[6].attenuation, 0u);
    device.setGain(SOAPY_SDR_TX, 0, 0);
    REQUIRE_EQ(sdk.state[6].attenuation, std::numeric_limits<std::uint16_t>::max());
    REQUIRE_EQ(device.getGain(SOAPY_SDR_TX, 0), 0.0);
}

TEST_CASE("RX gain readback rejects indices outside the SDK range and metadata failures")
{
    for (const auto &profile : profiles)
    {
        SoapySidekiq device;
        device.configure(profile);
        sdk.state[3].gain =
            static_cast<std::uint8_t>(profile.offset == 0 ? 77 : profile.offset - 1);
        REQUIRE_THROWS_AS(device.getGain(SOAPY_SDR_RX, 0), std::runtime_error);
        sdk.fail_on = 'R';
        REQUIRE_THROWS_AS(device.getGain(SOAPY_SDR_RX, 0), std::runtime_error);
        REQUIRE_EQ(sdk.events.back(), 'R');
    }
}

TEST_CASE("unknown named gain elements fail before SDK access or mode changes")
{
    SoapySidekiq device;
    sdk.state[3].mode = skiq_rx_gain_auto;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const std::string name : {"", "lna", "unknown"})
        {
            REQUIRE_THROWS_AS(device.setGain(direction, 0, name, 5), std::invalid_argument);
            REQUIRE_THROWS_AS(device.getGain(direction, 0, name), std::invalid_argument);
            REQUIRE_THROWS_AS(device.getGainRange(direction, 0, name), std::invalid_argument);
        }
    REQUIRE(sdk.events.empty());
    REQUIRE(sdk.state[3].mode == skiq_rx_gain_auto);
}
TEST_CASE("every advertised gain element works with named read write and range APIs")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (std::size_t channel = 0; channel < 2; ++channel)
            for (const auto &name : device.listGains(direction, channel))
            {
                device.setGain(direction, channel, name, 1.25);
                REQUIRE_EQ(device.getGain(direction, channel, name),
                           direction == SOAPY_SDR_RX ? 1.0 : 1.25);
                REQUIRE_EQ(device.getGainRange(direction, channel, name).maximum(),
                           direction == SOAPY_SDR_RX ? 76.0 : 89.75);
            }
}
TEST_CASE("named gain validation rejects unavailable channels before consulting hardware")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.setGain(9, 0, "LNA", 5), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getGain(9, 0, "LNA"), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getGainRange(9, 0, "LNA"), std::invalid_argument);
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const std::size_t channel : {std::size_t{2}, std::numeric_limits<std::size_t>::max()})
        {
            REQUIRE_THROWS_AS(device.setGain(direction, channel, "unknown", 5), std::out_of_range);
            REQUIRE_THROWS_AS(device.getGain(direction, channel, "unknown"), std::out_of_range);
            REQUIRE_THROWS_AS(device.getGainRange(direction, channel, "unknown"),
                              std::out_of_range);
        }
    REQUIRE(sdk.events.empty());
}
