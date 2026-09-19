#include "SidekiqUtils.hpp"
#include <SoapySDR/Formats.hpp>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>
#include <string>
#include <sidekiq_types.h>
#include <unistd.h>

namespace sidekiq
{
// compares two strings and if equal range and equal values per character
// returns true.
bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    return std::equal(a.begin(), a.end(), b.begin(),
        [](char lhs, char rhs)
        {
            return std::tolower(static_cast<unsigned char>(lhs)) ==
                   std::tolower(static_cast<unsigned char>(rhs));
        });
}

// skiq_param_t::rx_param[] and tx_param[] are indexed by channel
// (0 .. rf_param.num_*_channels), not by handle.  On most cards the two are
// the same, but that is not guaranteed (e.g. with a topology applied), so
// translate the handle through rf_param before indexing.
size_t rxParamIndexForHandle(const skiq_param_t &param, const skiq_rx_hdl_t handle)
{
    if (handle >= skiq_rx_hdl_end)
    {
        throw std::runtime_error("invalid RX handle " +
                                 std::to_string(static_cast<int>(handle)));
    }

    for (uint8_t index = 0; index < param.rf_param.num_rx_channels; index++)
    {
        if (param.rf_param.rx_handles[index] == handle)
        {
            return index;
        }
    }

    return static_cast<size_t>(handle);
}

size_t txParamIndexForHandle(const skiq_param_t &param, const skiq_tx_hdl_t handle)
{
    if (handle >= skiq_tx_hdl_end)
    {
        throw std::runtime_error("invalid TX handle " +
                                 std::to_string(static_cast<int>(handle)));
    }

    for (uint8_t index = 0; index < param.rf_param.num_tx_channels; index++)
    {
        if (param.rf_param.tx_handles[index] == handle)
        {
            return index;
        }
    }

    return static_cast<size_t>(handle);
}

bool rxHandleMayShareLo(const skiq_rx_hdl_t handle)
{
    return handle == skiq_rx_hdl_A2 || handle == skiq_rx_hdl_B2;
}

const char *rxHandleName(const skiq_rx_hdl_t handle)
{
    switch (handle)
    {
        case skiq_rx_hdl_A1: return "A1";
        case skiq_rx_hdl_A2: return "A2";
        case skiq_rx_hdl_B1: return "B1";
        case skiq_rx_hdl_B2: return "B2";
        case skiq_rx_hdl_C1: return "C1";
        case skiq_rx_hdl_D1: return "D1";
        default: return "unknown";
    }
}

const char *txHandleName(const skiq_tx_hdl_t handle)
{
    switch (handle)
    {
        case skiq_tx_hdl_A1: return "A1";
        case skiq_tx_hdl_A2: return "A2";
        case skiq_tx_hdl_B1: return "B1";
        case skiq_tx_hdl_B2: return "B2";
        default: return "unknown";
    }
}

bool isRxGainName(const std::string &name)
{
    return equalsIgnoreCase(name, "LNA") ||
           equalsIgnoreCase(name, "gain") ||
           equalsIgnoreCase(name, "rx_gain");
}

bool isTxOutputGainName(const std::string &name)
{
    return equalsIgnoreCase(name, "gain") ||
           equalsIgnoreCase(name, "output_gain") ||
           equalsIgnoreCase(name, "tx_gain") ||
           equalsIgnoreCase(name, "LNA");
}

bool isTxAttenuationName(const std::string &name)
{
    return equalsIgnoreCase(name, "attenuation") ||
           equalsIgnoreCase(name, "attenuator") ||
           equalsIgnoreCase(name, "attn") ||
           equalsIgnoreCase(name, "tx_attenuation");
}

RxGainIndexRange readRxGainIndexRange(const uint8_t card,
                                      const skiq_rx_hdl_t handle)
{
    RxGainIndexRange range{0, 0};
    const int status = skiq_read_rx_gain_index_range(card,
                                                     handle,
                                                     &range.minimum,
                                                     &range.maximum);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_rx_gain_index_range failed "
                      "(card %u, handle %u), status %d",
                      card,
                      handle,
                      status);
        throw std::runtime_error("");
    }

    if (range.maximum < range.minimum)
    {
        throw std::runtime_error("Sidekiq SDK returned an invalid RX gain index range");
    }

    return range;
}

double rxGainStepDb(const skiq_part_t part, const skiq_rx_hdl_t handle)
{
    switch (part)
    {
        case skiq_x2:
            return handle == skiq_rx_hdl_B1 ? 1.0 : 0.5;
        case skiq_x4:
#if SOAPYSIDEKIQ_HAS_SDK_X40_PART
        case skiq_x40:
#endif
        case skiq_nv100:
#if SOAPYSIDEKIQ_HAS_SDK_NVM2_PART
        case skiq_nvm2:
#endif
#if SOAPYSIDEKIQ_HAS_SDK_Z4_PART
        case skiq_z4:
        case skiq_z4_mp:
#endif
            return 0.5;
        case skiq_mpcie:
        case skiq_m2:
        case skiq_m2_2280:
        case skiq_z2:
        case skiq_z3u:
        default:
            return 1.0;
    }
}

double rxGainDbFromIndex(const skiq_part_t part,
                         const skiq_rx_hdl_t handle,
                         const RxGainIndexRange &range,
                         const uint8_t gain_index)
{
    const uint8_t clamped_index =
        std::max(range.minimum, std::min(range.maximum, gain_index));
    return static_cast<double>(clamped_index - range.minimum) *
           rxGainStepDb(part, handle);
}

uint8_t rxGainIndexFromDb(const skiq_part_t part,
                          const skiq_rx_hdl_t handle,
                          const RxGainIndexRange &range,
                          const double gain_db)
{
    const double step = rxGainStepDb(part, handle);
    const int requested =
        static_cast<int>(range.minimum) +
        static_cast<int>(std::llround(gain_db / step));
    const int clamped = std::max(static_cast<int>(range.minimum),
                                 std::min(static_cast<int>(range.maximum),
                                          requested));
    return static_cast<uint8_t>(clamped);
}

SoapySDR::Range rxGainRangeDb(const skiq_part_t part,
                              const skiq_rx_hdl_t handle,
                              const RxGainIndexRange &range)
{
    const double step = rxGainStepDb(part, handle);
    return SoapySDR::Range(0.0,
                           static_cast<double>(range.maximum - range.minimum) *
                               step,
                           step);
}

TxAttenuationIndexRange txAttenuationIndexRange(const skiq_param_t &param,
                                                const skiq_tx_hdl_t handle)
{
    const skiq_tx_param_t &tx_param = txParamForHandle(param, handle);
    TxAttenuationIndexRange range{
        tx_param.atten_quarter_db_min,
        tx_param.atten_quarter_db_max
    };

    if (range.maximum < range.minimum)
    {
        throw std::runtime_error("Sidekiq SDK returned an invalid TX attenuation range");
    }

    return range;
}

double quarterDbToDb(const uint16_t value)
{
    return static_cast<double>(value) * TX_ATTENUATION_STEP_DB;
}

uint16_t dbToQuarterDb(const double value)
{
    return static_cast<uint16_t>(std::llround(value / TX_ATTENUATION_STEP_DB));
}

bool rangeContains(const SoapySDR::Range &range, const double value)
{
    return std::isfinite(value) &&
           value >= range.minimum() &&
           value <= range.maximum();
}

SoapySDR::Range txAttenuationRangeDb(const skiq_param_t &param,
                                     const skiq_tx_hdl_t handle)
{
    const TxAttenuationIndexRange range =
        txAttenuationIndexRange(param, handle);
    return SoapySDR::Range(quarterDbToDb(range.minimum),
                           quarterDbToDb(range.maximum),
                           TX_ATTENUATION_STEP_DB);
}

SoapySDR::Range txOutputGainRangeDb(const skiq_param_t &param,
                                    const skiq_tx_hdl_t handle)
{
    const TxAttenuationIndexRange range =
        txAttenuationIndexRange(param, handle);
    return SoapySDR::Range(0.0,
                           quarterDbToDb(range.maximum - range.minimum),
                           TX_ATTENUATION_STEP_DB);
}

uint16_t txAttenuationIndexFromOutputGainDb(const skiq_param_t &param,
                                            const skiq_tx_hdl_t handle,
                                            const double gain_db)
{
    const TxAttenuationIndexRange range =
        txAttenuationIndexRange(param, handle);
    const int requested =
        static_cast<int>(range.maximum) -
        static_cast<int>(std::llround(gain_db / TX_ATTENUATION_STEP_DB));
    const int clamped = std::max(static_cast<int>(range.minimum),
                                 std::min(static_cast<int>(range.maximum),
                                          requested));
    return static_cast<uint16_t>(clamped);
}

uint16_t txAttenuationIndexFromAttenuationDb(const skiq_param_t &param,
                                             const skiq_tx_hdl_t handle,
                                             const double attenuation_db)
{
    const TxAttenuationIndexRange range =
        txAttenuationIndexRange(param, handle);
    const int requested = static_cast<int>(dbToQuarterDb(attenuation_db));
    const int clamped = std::max(static_cast<int>(range.minimum),
                                 std::min(static_cast<int>(range.maximum),
                                          requested));
    return static_cast<uint16_t>(clamped);
}

double txOutputGainDbFromAttenuationIndex(const skiq_param_t &param,
                                          const skiq_tx_hdl_t handle,
                                          const uint16_t attenuation_index)
{
    const TxAttenuationIndexRange range =
        txAttenuationIndexRange(param, handle);
    const uint16_t clamped_index =
        std::max(range.minimum, std::min(range.maximum, attenuation_index));
    return quarterDbToDb(range.maximum - clamped_index);
}

double txAttenuationDbFromAttenuationIndex(const skiq_param_t &param,
                                           const skiq_tx_hdl_t handle,
                                           const uint16_t attenuation_index)
{
    const TxAttenuationIndexRange range =
        txAttenuationIndexRange(param, handle);
    const uint16_t clamped_index =
        std::max(range.minimum, std::min(range.maximum, attenuation_index));
    return quarterDbToDb(clamped_index);
}

std::string rfPortName(const skiq_rf_port_t port)
{
    switch (port)
    {
        case skiq_rf_port_J1: return "J1";
        case skiq_rf_port_J2: return "J2";
        case skiq_rf_port_J3: return "J3";
        case skiq_rf_port_J4: return "J4";
        case skiq_rf_port_J5: return "J5";
        case skiq_rf_port_J6: return "J6";
        case skiq_rf_port_J7: return "J7";
        case skiq_rf_port_J300: return "J300";
        case skiq_rf_port_Jxxx_RX1: return "RX1";
        case skiq_rf_port_Jxxx_TX1RX2: return "TX1RX2";
        case skiq_rf_port_J8: return "J8";
        case skiq_rf_port_unknown: return "NONE";
        default:
        {
            // RF ports added in newer SDKs: use libsidekiq's name for them
            const char *sdk_name = skiq_rf_port_string(port);
            return (sdk_name != nullptr && sdk_name[0] != '\0') ? sdk_name : "NONE";
        }
    }
}

bool rfPortNameMatches(const std::string &requested,
                       const skiq_rf_port_t port)
{
    if (equalsIgnoreCase(requested, rfPortName(port)))
    {
        return true;
    }

    const char *sdk_name = skiq_rf_port_string(port);
    return sdk_name != nullptr && equalsIgnoreCase(requested, sdk_name);
}

void appendRfPortName(std::vector<std::string> &names,
                      const skiq_rf_port_t port)
{
    const std::string name = rfPortName(port);
    if (name == "NONE")
    {
        return;
    }

    if (std::find(names.begin(), names.end(), name) == names.end())
    {
        names.push_back(name);
    }
}

bool findPortInList(const skiq_rf_port_t requested,
                    const skiq_rf_port_t *ports,
                    const uint8_t count)
{
    for (uint8_t index = 0; index < count; index++)
    {
        if (ports[index] == requested)
        {
            return true;
        }
    }

    return false;
}

skiq_rf_port_t findSinglePortByAlias(const std::string &name,
                                     const skiq_rf_port_t *fixed_ports,
                                     const uint8_t num_fixed_ports,
                                     const skiq_rf_port_t *trx_ports,
                                     const uint8_t num_trx_ports)
{
    if (equalsIgnoreCase(name, "TRX"))
    {
        return num_trx_ports == 1 ? trx_ports[0] : skiq_rf_port_unknown;
    }

    if (equalsIgnoreCase(name, "RX") || equalsIgnoreCase(name, "TX"))
    {
        return num_fixed_ports == 1 ? fixed_ports[0] : skiq_rf_port_unknown;
    }

    return skiq_rf_port_unknown;
}

skiq_rf_port_t rfPortFromAntennaName(const std::string &name,
                                     const skiq_rf_port_t *fixed_ports,
                                     const uint8_t num_fixed_ports,
                                     const skiq_rf_port_t *trx_ports,
                                     const uint8_t num_trx_ports)
{
    for (uint8_t index = 0; index < num_fixed_ports; index++)
    {
        if (rfPortNameMatches(name, fixed_ports[index]))
        {
            return fixed_ports[index];
        }
    }

    for (uint8_t index = 0; index < num_trx_ports; index++)
    {
        if (rfPortNameMatches(name, trx_ports[index]))
        {
            return trx_ports[index];
        }
    }

    return findSinglePortByAlias(name,
                                 fixed_ports,
                                 num_fixed_ports,
                                 trx_ports,
                                 num_trx_ports);
}

std::string hzString(const double value)
{
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

std::string rangeString(const SoapySDR::Range &range)
{
    return "[" + hzString(range.minimum()) + ", " +
           hzString(range.maximum()) + "] Hz";
}

void requirePositiveHz(const std::string &what, const double value)
{
    if (!std::isfinite(value) || value <= 0.0)
    {
        throw std::runtime_error(what + " must be a positive value in Hz");
    }
}

uint32_t hzToUint32(const std::string &what, const double value)
{
    requirePositiveHz(what, value);
    if (value > static_cast<double>(std::numeric_limits<uint32_t>::max()))
    {
        throw std::runtime_error(what + " is too large for the Sidekiq API: " +
                                 hzString(value));
    }
    return static_cast<uint32_t>(value);
}

uint64_t hzToUint64(const std::string &what, const double value)
{
    requirePositiveHz(what, value);
    if (value > static_cast<double>(std::numeric_limits<uint64_t>::max()))
    {
        throw std::runtime_error(what + " is too large for the Sidekiq API: " +
                                 hzString(value));
    }
    return static_cast<uint64_t>(value);
}

SoapySDR::Range fallbackSampleRateRange(const uint8_t card)
{
    uint32_t min_sample_rate = 0;
    uint32_t max_sample_rate = 0;

    int status = skiq_read_min_sample_rate(card, &min_sample_rate);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_min_sample_rate failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }

    status = skiq_read_max_sample_rate(card, &max_sample_rate);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_max_sample_rate failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }

    return SoapySDR::Range(min_sample_rate, max_sample_rate);
}

SoapySDR::Range sampleRateRangeFromValues(const uint8_t card,
                                          const uint32_t min_sample_rate,
                                          const uint32_t max_sample_rate)
{
    if (min_sample_rate != 0 && max_sample_rate >= min_sample_rate)
    {
        return SoapySDR::Range(min_sample_rate, max_sample_rate);
    }

    return fallbackSampleRateRange(card);
}

SoapySDR::Range rxSampleRateRangeForHandle(const uint8_t card,
                                           const skiq_param_t &param,
                                           const skiq_rx_hdl_t handle)
{
    const skiq_rx_param_t &rx_param = rxParamForHandle(param, handle);
    return sampleRateRangeFromValues(card,
                                     rx_param.sample_rate_min,
                                     rx_param.sample_rate_max);
}

SoapySDR::Range txSampleRateRangeForHandle(const uint8_t card,
                                           const skiq_param_t &param,
                                           const skiq_tx_hdl_t handle)
{
    const skiq_tx_param_t &tx_param = txParamForHandle(param, handle);
    return sampleRateRangeFromValues(card,
                                     tx_param.sample_rate_min,
                                     tx_param.sample_rate_max);
}

SoapySDR::Range intersectSampleRateRanges(const uint8_t card,
                                          const skiq_param_t &param,
                                          const std::vector<skiq_rx_hdl_t> &handles)
{
    if (handles.empty())
    {
        throw std::runtime_error("cannot compute RX sample-rate range without handles");
    }

    SoapySDR::Range intersection =
        rxSampleRateRangeForHandle(card, param, handles.front());

    for (size_t index = 1; index < handles.size(); index++)
    {
        const SoapySDR::Range next =
            rxSampleRateRangeForHandle(card, param, handles[index]);
        const double minimum = std::max(intersection.minimum(), next.minimum());
        const double maximum = std::min(intersection.maximum(), next.maximum());
        if (minimum > maximum)
        {
            throw std::runtime_error(
                "selected RX handles do not share a common sample-rate range");
        }
        intersection = SoapySDR::Range(minimum, maximum);
    }

    return intersection;
}

void validateRangeValue(const std::string &what,
                        const double value,
                        const SoapySDR::Range &range)
{
    if (value < range.minimum() || value > range.maximum())
    {
        throw std::runtime_error(what + " " + hzString(value) +
                                 " Hz is outside the supported range " +
                                 rangeString(range));
    }
}

std::vector<double> steppedValuesForRanges(const SoapySDR::RangeList &ranges,
                                           const double step)
{
    std::vector<double> results;

    for (const auto &range : ranges)
    {
        for (double value = range.minimum(); value <= range.maximum();
             value += step)
        {
            results.push_back(value);
        }

        if (results.empty() || results.back() != range.maximum())
        {
            results.push_back(range.maximum());
        }
    }

    return results;
}

uint32_t rxHandleMask(const skiq_rx_hdl_t handle)
{
    return handle < 32 ? (1u << static_cast<unsigned>(handle)) : 0;
}

uint32_t txHandleMask(const skiq_tx_hdl_t handle)
{
    return handle < 32 ? (1u << static_cast<unsigned>(handle)) : 0;
}

bool libsidekiqAtLeast(const uint8_t major,
                       const uint8_t minor,
                       const uint8_t patch)
{
    uint8_t actual_major = LIBSIDEKIQ_VERSION_MAJOR;
    uint8_t actual_minor = LIBSIDEKIQ_VERSION_MINOR;
    uint8_t actual_patch = LIBSIDEKIQ_VERSION_PATCH;
    const char *label = nullptr;

    const int status = skiq_read_libsidekiq_version(&actual_major,
                                                    &actual_minor,
                                                    &actual_patch,
                                                    &label);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_WARNING,
                      "skiq_read_libsidekiq_version failed, status %d; "
                      "using compile-time libsidekiq version",
                      status);
    }

    if (actual_major != major) return actual_major > major;
    if (actual_minor != minor) return actual_minor > minor;
    return actual_patch >= patch;
}

std::vector<ProfileEntry> nv100Profiles(const bool include_v424_rates,
                                        const uint32_t handle_mask)
{
    std::vector<ProfileEntry> profiles = {
        profile(250000, handle_mask, 0),
        profile(541667, handle_mask, 0),
        profile(740740, handle_mask, 0),
        profile(750000, handle_mask, 0),
        profile(1000000, handle_mask, 0),
        profile(1920000, handle_mask, 0),
        profile(2457600, handle_mask, 0),
        profile(2500000, handle_mask, 0),
        profile(2800000, handle_mask, 0),
        profile(3840000, handle_mask, 0),
        profile(4000000, handle_mask, 0),
        profile(4915200, handle_mask, 0),
        profile(5000000, handle_mask, 0),
        profile(5600000, handle_mask, 0),
        profile(7680000, handle_mask, 0),
        profile(9830400, handle_mask, 0),
        profile(10000000, handle_mask, 0),
        profile(11200000, handle_mask, 0),
        profile(15360000, handle_mask, 0),
        profile(16000000, handle_mask, 0),
        profile(20000000, handle_mask, 0),
        profile(21666700, handle_mask, 0),
        profile(22000000, handle_mask, 0),
        profile(23040000, handle_mask, 0),
        profile(30720000, handle_mask, 0),
        profile(40000000, handle_mask, 0),
        profile(61440000, handle_mask, 0)
    };

#if defined(LIBSIDEKIQ_VERSION) && (LIBSIDEKIQ_VERSION >= 42600)
    // listed in the v4.26 SDK manual's NV100/NVM2/G20/G40/Z4 rate table
    profiles.push_back(profile(625000, handle_mask, 0));
#endif

    if (include_v424_rates)
    {
        profiles.push_back(profile(160000, handle_mask, 0));
        profiles.push_back(profile(270270, handle_mask, 0));
        profiles.push_back(profile(307200, handle_mask, 0));
        profiles.push_back(profile(640000, handle_mask, 0));
        profiles.push_back(profile(50000000, handle_mask, 0));
        profiles.push_back(profile(60000000, handle_mask, 0));
    }

    return profiles;
}

std::vector<ProfileEntry> rxProfilesForPart(const skiq_part_t part)
{
    switch (part)
    {
        case skiq_x2:
            return {
                profile(245760000, RX_B1, 200000000, 100000000),
                profile(153600000, RX_ALL_X2, 100000000),
                profile(122880000, RX_ALL_X2, 100000000),
                profile(100000000, RX_ALL_X2, 82000000),
                profile(73728000, RX_ALL_X2, 60456000, 30228000),
                profile(61440000, RX_ALL_X2, 50000000, 25000000),
                profile(50000000, RX_ALL_X2, 41000000),
                profile(36864000, RX_A1 | RX_A2, 30228000),
                profile(30720000, RX_A1 | RX_A2, 25000000, 20000000, 18000000)
            };

        case skiq_x4:
#if SOAPYSIDEKIQ_HAS_SDK_X40_PART
        case skiq_x40:
#endif
            return {
                profile(500000000, RX_C1 | RX_D1, 450000000, 400000000),
                profile(491520000, RX_C1 | RX_D1, 450000000, 400000000),
                profile(250000000, RX_ALL_X4, 200000000, 100000000),
                profile(245760000, RX_ALL_X4, 200000000, 100000000),
                profile(200000000, RX_ALL_X4, 164000000),
                profile(153600000, RX_ALL_X4, 100000000),
                profile(122880000, RX_ALL_X4, 100000000, 72000000, 64000000, 61440000),
                profile(100000000, RX_ALL_X4, 82000000),
                profile(76800000, RX_ALL_X4, 30720000),
                profile(73728000, RX_ALL_X4, 60456000, 30228000),
                profile(61440000, RX_ALL_X4, 50000000, 25000000),
                profile(50000000, RX_ALL_X4, 41000000, 20000000)
            };

        case skiq_nv100:
#if SOAPYSIDEKIQ_HAS_SDK_NVM2_PART
        case skiq_nvm2:
#endif
        {
            std::vector<ProfileEntry> profiles =
                nv100Profiles(libsidekiqAtLeast(4, 24, 0), RX_ALL_NV100);
            // "disparate" rate: RX at 1.4 MS/s while TX runs at 5.6 MS/s
            // (SDK manual, NV100/NVM2/G20/G40/Z4 disparate sample rates)
            if (libsidekiqAtLeast(4, 17, 0))
            {
                profiles.push_back(profile(1400000, RX_ALL_NV100, 0));
            }
            return profiles;
        }

        default:
            return {};
    }
}

std::vector<ProfileEntry> txProfilesForPart(const skiq_part_t part)
{
    switch (part)
    {
        case skiq_x2:
            return {
                profile(153600000, TX_ALL_X2, 100000000),
                profile(122880000, TX_ALL_X2, 100000000),
                profile(100000000, TX_ALL_X2, 82000000),
                profile(73728000, TX_ALL_X2, 60456000),
                profile(61440000, TX_ALL_X2, 50000000),
                profile(50000000, TX_ALL_X2, 41000000)
            };

        case skiq_x4:
            return {
                profile(500000000, TX_ALL_X4, 450000000, 400000000),
                profile(491520000, TX_ALL_X4, 450000000, 400000000),
                profile(250000000, TX_ALL_X4, 200000000, 100000000),
                profile(245760000, TX_ALL_X4, 200000000, 100000000),
                profile(200000000, TX_ALL_X4, 164000000),
                profile(153600000, TX_ALL_X4, 100000000),
                profile(122880000, TX_ALL_X4, 100000000, 72000000, 64000000, 61440000),
                profile(100000000, TX_ALL_X4, 82000000),
                profile(76800000, TX_ALL_X4, 30720000),
                profile(73728000, TX_ALL_X4, 60456000, 30228000),
                profile(61440000, TX_ALL_X4, 50000000, 25000000),
                profile(50000000, TX_ALL_X4, 41000000, 20000000)
            };

#if SOAPYSIDEKIQ_HAS_SDK_X40_PART
        case skiq_x40:
            return {
                profile(500000000, TX_A1 | TX_B1, 450000000, 400000000),
                profile(491520000, TX_A1 | TX_B1, 450000000, 400000000),
                profile(250000000, TX_A1 | TX_B1, 200000000, 100000000),
                profile(245760000, TX_A1 | TX_B1, 200000000, 100000000),
                profile(200000000, TX_A1 | TX_B1, 164000000),
                profile(153600000, TX_A1 | TX_B1, 100000000),
                profile(122880000, TX_A1 | TX_B1, 100000000, 72000000, 64000000, 61440000),
                profile(100000000, TX_A1 | TX_B1, 82000000),
                profile(76800000, TX_A1 | TX_B1, 30720000),
                profile(73728000, TX_A1 | TX_B1, 60456000, 30228000),
                profile(61440000, TX_A1 | TX_B1, 50000000, 25000000),
                profile(50000000, TX_A1 | TX_B1, 41000000, 20000000)
            };
#endif

        case skiq_nv100:
#if SOAPYSIDEKIQ_HAS_SDK_NVM2_PART
        case skiq_nvm2:
#endif
            return nv100Profiles(libsidekiqAtLeast(4, 24, 0), TX_ALL_NV100);

        default:
            return {};
    }
}

bool appendUniqueDouble(std::vector<double> &values, const double value)
{
    if (std::find(values.begin(), values.end(), value) != values.end())
    {
        return false;
    }

    values.push_back(value);
    return true;
}

std::vector<double> sampleRatesFromProfiles(const std::vector<ProfileEntry> &profiles,
                                            const uint32_t handle_mask)
{
    std::vector<double> values;
    for (const auto &entry : profiles)
    {
        if ((entry.handle_mask & handle_mask) != 0)
        {
            appendUniqueDouble(values, entry.rate);
        }
    }

    std::sort(values.begin(), values.end());
    return values;
}

std::vector<double> filterValuesInRange(const std::vector<double> &values,
                                        const SoapySDR::Range &range)
{
    std::vector<double> filtered;
    for (const double value : values)
    {
        if (value >= range.minimum() && value <= range.maximum())
        {
            appendUniqueDouble(filtered, value);
        }
    }

    std::sort(filtered.begin(), filtered.end());
    return filtered;
}

bool rateMatchesProfile(const std::vector<double> &rates, const uint32_t rate)
{
    for (const double profile_rate : rates)
    {
        if (std::abs(profile_rate - static_cast<double>(rate)) <= 100.0)
        {
            return true;
        }
    }

    return false;
}

std::vector<double> rxProfileSampleRates(const skiq_part_t part,
                                         const uint8_t card,
                                         const skiq_param_t &param,
                                         const skiq_rx_hdl_t handle)
{
    return filterValuesInRange(
        sampleRatesFromProfiles(rxProfilesForPart(part), rxHandleMask(handle)),
        rxSampleRateRangeForHandle(card, param, handle));
}

std::vector<double> txProfileSampleRates(const skiq_part_t part,
                                         const uint8_t card,
                                         const skiq_param_t &param,
                                         const skiq_tx_hdl_t handle)
{
    return filterValuesInRange(
        sampleRatesFromProfiles(txProfilesForPart(part), txHandleMask(handle)),
        txSampleRateRangeForHandle(card, param, handle));
}

std::vector<double> bandwidthsFromProfilesForRate(
        const std::vector<ProfileEntry> &profiles,
        const uint32_t handle_mask,
        const uint32_t sample_rate)
{
    std::vector<double> values;
    for (const auto &entry : profiles)
    {
        if ((entry.handle_mask & handle_mask) == 0)
        {
            continue;
        }

        if (std::abs(static_cast<double>(entry.rate) -
                     static_cast<double>(sample_rate)) > 100.0)
        {
            continue;
        }

        for (uint8_t index = 0; index < entry.bandwidth_count; index++)
        {
            appendUniqueDouble(values, entry.bandwidths[index]);
        }
    }

    std::sort(values.begin(), values.end());
    return values;
}

bool partRequiresExactBuiltInSampleRate(const skiq_part_t part)
{
    return part == skiq_nv100
#if SOAPYSIDEKIQ_HAS_SDK_NVM2_PART
        || part == skiq_nvm2
#endif
        ;
}

void validateBuiltInSampleRateIfRequired(const std::string &what,
                                         const skiq_part_t part,
                                         const uint32_t rate,
                                         const std::vector<double> &rates)
{
    if (!partRequiresExactBuiltInSampleRate(part) || rates.empty())
    {
        return;
    }

    if (rateMatchesProfile(rates, rate))
    {
        return;
    }

    std::ostringstream stream;
    stream << what << " " << rate
           << " Hz is not one of the documented built-in profile rates for "
           << skiq_part_string(part) << ". Use listSampleRates() to query "
           << "the suggested rates for this card.";
    throw std::runtime_error(stream.str());
}

std::vector<double> nv100BandwidthsForRate(const uint32_t rate)
{
    std::vector<double> values;

    appendUniqueDouble(values, rate * 0.03);
    for (double percent = 5.0; percent <= 80.0; percent += 0.5)
    {
        appendUniqueDouble(values, rate * (percent / 100.0));
    }

    appendUniqueDouble(values, rate * 0.86);
    appendUniqueDouble(values, rate * 0.89);
    appendUniqueDouble(values, rate * 0.95);
    appendUniqueDouble(values, rate * 0.96);
    appendUniqueDouble(values, rate * 0.99);
    std::sort(values.begin(), values.end());
    return values;
}

bool bandwidthDiffersSignificantly(const uint32_t requested, const uint32_t actual)
{
    const double difference =
        std::abs(static_cast<double>(actual) - static_cast<double>(requested));
    return requested == 0 || difference > 0.05 * static_cast<double>(requested);
}

void validateBandwidthAgainstSampleRate(const std::string &what,
                                        const uint32_t bandwidth,
                                        const uint32_t sample_rate)
{
    if (bandwidth == 0)
    {
        throw std::runtime_error(what + " must be a positive value in Hz");
    }

    if (sample_rate != 0 && bandwidth > sample_rate)
    {
        throw std::runtime_error(what + " " + std::to_string(bandwidth) +
                                 " Hz exceeds the current sample rate " +
                                 std::to_string(sample_rate) + " Hz");
    }
}

const skiq_rx_param_t &rxParamForHandle(const skiq_param_t &param,
                                        const skiq_rx_hdl_t handle)
{
    return param.rx_param[rxParamIndexForHandle(param, handle)];
}

const skiq_tx_param_t &txParamForHandle(const skiq_param_t &param,
                                        const skiq_tx_hdl_t handle)
{
    return param.tx_param[txParamIndexForHandle(param, handle)];
}
}
