#pragma once

// Internal helpers shared by the SoapySidekiq sources: handle and parameter
// lookups, frequency/rate validation, gain and attenuation conversions, and
// the built-in RFIC sample-rate profile tables.

#include <sidekiq_api.h>

#include <cstdint>
#include <string>
#include <vector>

#include <SoapySDR/Types.hpp>

#include "SoapySidekiq.hpp"

namespace sidekiq
{
struct RxGainIndexRange
{
    uint8_t minimum;
    uint8_t maximum;
};

constexpr double TX_ATTENUATION_STEP_DB = 0.25;

struct TxAttenuationIndexRange
{
    uint16_t minimum;
    uint16_t maximum;
};

constexpr uint32_t RX_A1 = (1u << skiq_rx_hdl_A1);
constexpr uint32_t RX_A2 = (1u << skiq_rx_hdl_A2);
constexpr uint32_t RX_B1 = (1u << skiq_rx_hdl_B1);
constexpr uint32_t RX_B2 = (1u << skiq_rx_hdl_B2);
constexpr uint32_t RX_C1 = (1u << skiq_rx_hdl_C1);
constexpr uint32_t RX_D1 = (1u << skiq_rx_hdl_D1);
constexpr uint32_t RX_ALL_X2 = RX_A1 | RX_A2 | RX_B1;
constexpr uint32_t RX_ALL_X4 = RX_A1 | RX_A2 | RX_B1 | RX_B2 | RX_C1 | RX_D1;
constexpr uint32_t RX_ALL_NV100 = RX_A1 | RX_A2 | RX_B1 | RX_B2;
constexpr uint32_t TX_A1 = (1u << skiq_tx_hdl_A1);
constexpr uint32_t TX_A2 = (1u << skiq_tx_hdl_A2);
constexpr uint32_t TX_B1 = (1u << skiq_tx_hdl_B1);
constexpr uint32_t TX_B2 = (1u << skiq_tx_hdl_B2);
constexpr uint32_t TX_ALL_X2 = TX_A1 | TX_A2;
constexpr uint32_t TX_ALL_X4 = TX_A1 | TX_A2 | TX_B1 | TX_B2;
constexpr uint32_t TX_ALL_NV100 = TX_A1 | TX_A2 | TX_B1 | TX_B2;

struct ProfileEntry
{
    uint32_t rate;
    uint32_t handle_mask;
    uint8_t bandwidth_count;
    uint32_t bandwidths[4];
};

constexpr ProfileEntry profile(const uint32_t rate,
                               const uint32_t handle_mask,
                               const uint32_t bw0,
                               const uint32_t bw1 = 0,
                               const uint32_t bw2 = 0,
                               const uint32_t bw3 = 0)
{
    return ProfileEntry{
        rate,
        handle_mask,
        static_cast<uint8_t>((bw0 != 0) + (bw1 != 0) + (bw2 != 0) + (bw3 != 0)),
        {bw0, bw1, bw2, bw3}
    };
}

// compares two strings and if equal range and equal values per character
// returns true.
bool equalsIgnoreCase(const std::string& a, const std::string& b);

// skiq_param_t::rx_param[] and tx_param[] are indexed by channel
// (0 .. rf_param.num_*_channels), not by handle.  On most cards the two are
// the same, but that is not guaranteed (e.g. with a topology applied), so
// translate the handle through rf_param before indexing.
size_t rxParamIndexForHandle(const skiq_param_t &param, const skiq_rx_hdl_t handle);

size_t txParamIndexForHandle(const skiq_param_t &param, const skiq_tx_hdl_t handle);

bool rxHandleMayShareLo(const skiq_rx_hdl_t handle);

const char *rxHandleName(const skiq_rx_hdl_t handle);

const char *txHandleName(const skiq_tx_hdl_t handle);

bool isRxGainName(const std::string &name);

bool isTxOutputGainName(const std::string &name);

bool isTxAttenuationName(const std::string &name);

RxGainIndexRange readRxGainIndexRange(const uint8_t card,
                                      const skiq_rx_hdl_t handle);

double rxGainStepDb(const skiq_part_t part, const skiq_rx_hdl_t handle);

double rxGainDbFromIndex(const skiq_part_t part,
                         const skiq_rx_hdl_t handle,
                         const RxGainIndexRange &range,
                         const uint8_t gain_index);

uint8_t rxGainIndexFromDb(const skiq_part_t part,
                          const skiq_rx_hdl_t handle,
                          const RxGainIndexRange &range,
                          const double gain_db);

SoapySDR::Range rxGainRangeDb(const skiq_part_t part,
                              const skiq_rx_hdl_t handle,
                              const RxGainIndexRange &range);

TxAttenuationIndexRange txAttenuationIndexRange(const skiq_param_t &param,
                                                const skiq_tx_hdl_t handle);

double quarterDbToDb(const uint16_t value);

uint16_t dbToQuarterDb(const double value);

bool rangeContains(const SoapySDR::Range &range, const double value);

SoapySDR::Range txAttenuationRangeDb(const skiq_param_t &param,
                                     const skiq_tx_hdl_t handle);

SoapySDR::Range txOutputGainRangeDb(const skiq_param_t &param,
                                    const skiq_tx_hdl_t handle);

uint16_t txAttenuationIndexFromOutputGainDb(const skiq_param_t &param,
                                            const skiq_tx_hdl_t handle,
                                            const double gain_db);

uint16_t txAttenuationIndexFromAttenuationDb(const skiq_param_t &param,
                                             const skiq_tx_hdl_t handle,
                                             const double attenuation_db);

double txOutputGainDbFromAttenuationIndex(const skiq_param_t &param,
                                          const skiq_tx_hdl_t handle,
                                          const uint16_t attenuation_index);

double txAttenuationDbFromAttenuationIndex(const skiq_param_t &param,
                                           const skiq_tx_hdl_t handle,
                                           const uint16_t attenuation_index);

std::string rfPortName(const skiq_rf_port_t port);

bool rfPortNameMatches(const std::string &requested,
                       const skiq_rf_port_t port);

void appendRfPortName(std::vector<std::string> &names,
                      const skiq_rf_port_t port);

bool findPortInList(const skiq_rf_port_t requested,
                    const skiq_rf_port_t *ports,
                    const uint8_t count);

skiq_rf_port_t findSinglePortByAlias(const std::string &name,
                                     const skiq_rf_port_t *fixed_ports,
                                     const uint8_t num_fixed_ports,
                                     const skiq_rf_port_t *trx_ports,
                                     const uint8_t num_trx_ports);

skiq_rf_port_t rfPortFromAntennaName(const std::string &name,
                                     const skiq_rf_port_t *fixed_ports,
                                     const uint8_t num_fixed_ports,
                                     const skiq_rf_port_t *trx_ports,
                                     const uint8_t num_trx_ports);

std::string hzString(const double value);

std::string rangeString(const SoapySDR::Range &range);

void requirePositiveHz(const std::string &what, const double value);

uint32_t hzToUint32(const std::string &what, const double value);

uint64_t hzToUint64(const std::string &what, const double value);

SoapySDR::Range fallbackSampleRateRange(const uint8_t card);

SoapySDR::Range sampleRateRangeFromValues(const uint8_t card,
                                          const uint32_t min_sample_rate,
                                          const uint32_t max_sample_rate);

SoapySDR::Range rxSampleRateRangeForHandle(const uint8_t card,
                                           const skiq_param_t &param,
                                           const skiq_rx_hdl_t handle);

SoapySDR::Range txSampleRateRangeForHandle(const uint8_t card,
                                           const skiq_param_t &param,
                                           const skiq_tx_hdl_t handle);

SoapySDR::Range intersectSampleRateRanges(const uint8_t card,
                                          const skiq_param_t &param,
                                          const std::vector<skiq_rx_hdl_t> &handles);

void validateRangeValue(const std::string &what,
                        const double value,
                        const SoapySDR::Range &range);

std::vector<double> steppedValuesForRanges(const SoapySDR::RangeList &ranges,
                                           const double step);

uint32_t rxHandleMask(const skiq_rx_hdl_t handle);

uint32_t txHandleMask(const skiq_tx_hdl_t handle);

bool libsidekiqAtLeast(const uint8_t major,
                       const uint8_t minor,
                       const uint8_t patch);

std::vector<ProfileEntry> nv100Profiles(const bool include_v424_rates,
                                        const uint32_t handle_mask);

std::vector<ProfileEntry> rxProfilesForPart(const skiq_part_t part);

std::vector<ProfileEntry> txProfilesForPart(const skiq_part_t part);

bool appendUniqueDouble(std::vector<double> &values, const double value);

std::vector<double> sampleRatesFromProfiles(const std::vector<ProfileEntry> &profiles,
                                            const uint32_t handle_mask);

std::vector<double> filterValuesInRange(const std::vector<double> &values,
                                        const SoapySDR::Range &range);

bool rateMatchesProfile(const std::vector<double> &rates, const uint32_t rate);

std::vector<double> rxProfileSampleRates(const skiq_part_t part,
                                         const uint8_t card,
                                         const skiq_param_t &param,
                                         const skiq_rx_hdl_t handle);

std::vector<double> txProfileSampleRates(const skiq_part_t part,
                                         const uint8_t card,
                                         const skiq_param_t &param,
                                         const skiq_tx_hdl_t handle);

std::vector<double> bandwidthsFromProfilesForRate(
        const std::vector<ProfileEntry> &profiles,
        const uint32_t handle_mask,
        const uint32_t sample_rate);

bool partRequiresExactBuiltInSampleRate(const skiq_part_t part);

void validateBuiltInSampleRateIfRequired(const std::string &what,
                                         const skiq_part_t part,
                                         const uint32_t rate,
                                         const std::vector<double> &rates);

std::vector<double> nv100BandwidthsForRate(const uint32_t rate);

bool bandwidthDiffersSignificantly(const uint32_t requested, const uint32_t actual);

void validateBandwidthAgainstSampleRate(const std::string &what,
                                        const uint32_t bandwidth,
                                        const uint32_t sample_rate);

// Look up the RX/TX parameters for a handle.  skiq_param_t's rx_param[] and
// tx_param[] arrays are indexed by channel, not by handle.
const skiq_rx_param_t &rxParamForHandle(const skiq_param_t &param,
                                        const skiq_rx_hdl_t handle);

const skiq_tx_param_t &txParamForHandle(const skiq_param_t &param,
                                        const skiq_tx_hdl_t handle);
}
