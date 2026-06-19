#include "SoapySidekiq.hpp"
#include <SoapySDR/Formats.hpp>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cinttypes>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>
#include <string>
#include <sidekiq_types.h>
#include <unistd.h>

/******************************************************************************/
/** This is the custom logging handler.  If there were custom handling
    required for logging messages, it should be handled here.

    @param signum: the signal number that occurred
    @return void
*/
void logging_handler( int32_t priority, const char *message )
{
    //printf("<PRIORITY %" PRIi32 "> custom logger: %s", priority, message);

    char* new_message = (char *)malloc(strlen(message) + 1);
    strcpy(new_message, message);

    // remove newline and or cr 
    size_t len = strlen(new_message);  // Get the length of the string
                               
    if (len > 0 && new_message[len - 1] == '\n') {
        new_message[len - 1] = '\0';  // Replace newline with null terminator
    }

    len = strlen(new_message);  // Get the length of the string
                            //
    if (len > 0 && new_message[len - 1] == '\r') {
        new_message[len - 1] = '\0';  // Replace newline with null terminator
    }

    switch (priority)
    {
        case SKIQ_LOG_DEBUG:
            SoapySDR_logf(SOAPY_SDR_DEBUG, "epiq-log: %s", new_message);
            break;

        case SKIQ_LOG_INFO:
            SoapySDR_logf(SOAPY_SDR_INFO, "epiq-log: %s", new_message);
            break;

        case SKIQ_LOG_WARNING:
            SoapySDR_logf(SOAPY_SDR_WARNING, "epiq-log: %s", new_message);
            break;

        case SKIQ_LOG_ERROR:
            SoapySDR_logf(SOAPY_SDR_ERROR, "epiq-log: %s", new_message);
            break;

        default:
            SoapySDR_logf(SOAPY_SDR_TRACE, "epiq-log undefined %s", new_message);
    }

    free(new_message);
}

/*****************************************************************************/
  /** This is the callback function for once the data has completed being sent.
      There is no guarantee that the complete callback will be in the order that
      the data was sent, this function just increments the completion count and
      signals the main thread that there is space available to send more packets.

      @param status status of the transmit packet completed
      @param p_block reference to the completed transmit block
      @param p_user reference to the user data
      @return void
 */
void SoapySidekiq::tx_complete(int32_t status, skiq_tx_block_t *p_data, uint32_t txIndex)
{
    this->complete_count++;

    // update the in use status of the packet just completed
    tx_buf_mutex.lock();
    if (p_tx_status[txIndex] != 1)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "status isn't 1");
    }
    p_tx_status[txIndex] = 0;
    tx_buf_mutex.unlock();

    // signal to the other thread that there may be space available now that a
    // packet send has completed
    {
        // Signal the condition variable
        pthread_mutex_lock(&space_avail_mutex);
        space_avail = true;
        pthread_cond_signal(&space_avail_cond);
        pthread_mutex_unlock(&space_avail_mutex);
    }
//    SoapySDR_logf(SOAPY_SDR_TRACE, "leaving tx_complete");

}

void SoapySidekiq::tx_enabled(uint8_t card, int32_t status)
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "tx enable received");

    // Signal the condition variable
    pthread_mutex_lock(&tx_enabled_mutex);
    pthread_cond_signal(&tx_enabled_cond);
    pthread_mutex_unlock(&tx_enabled_mutex);

}

std::vector<SoapySDR::Kwargs> SoapySidekiq::sidekiq_devices;
std::mutex SoapySidekiq::sidekiq_init_mutex;
bool SoapySidekiq::sidekiq_library_initialized = false;
unsigned SoapySidekiq::sidekiq_instance_count = 0;
unsigned SoapySidekiq::sidekiq_card_ref_count[SKIQ_MAX_NUM_CARDS] = {};

// compares two strings and if equal range and equal values per character
// returns true.
bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](char a, char b)
        {
            return std::tolower(a) == std::tolower(b);
        });
}

namespace
{
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

struct RxGainIndexRange
{
    uint8_t minimum;
    uint8_t maximum;
};

constexpr double TX_ATTENUATION_STEP_DB = 0.25;

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

struct TxAttenuationIndexRange
{
    uint16_t minimum;
    uint16_t maximum;
};

TxAttenuationIndexRange txAttenuationIndexRange(const skiq_param_t &param,
                                                const skiq_tx_hdl_t handle)
{
    if (handle >= skiq_tx_hdl_end)
    {
        throw std::runtime_error("invalid TX handle while reading attenuation range");
    }

    TxAttenuationIndexRange range{
        param.tx_param[handle].atten_quarter_db_min,
        param.tx_param[handle].atten_quarter_db_max
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
        default: return "NONE";
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
    if (handle >= skiq_rx_hdl_end)
    {
        throw std::runtime_error("invalid RX handle while reading sample-rate range");
    }

    const skiq_rx_param_t &rx_param = param.rx_param[handle];
    return sampleRateRangeFromValues(card,
                                     rx_param.sample_rate_min,
                                     rx_param.sample_rate_max);
}

SoapySDR::Range txSampleRateRangeForHandle(const uint8_t card,
                                           const skiq_param_t &param,
                                           const skiq_tx_hdl_t handle)
{
    if (handle >= skiq_tx_hdl_end)
    {
        throw std::runtime_error("invalid TX handle while reading sample-rate range");
    }

    const skiq_tx_param_t &tx_param = param.tx_param[handle];
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
            return nv100Profiles(libsidekiqAtLeast(4, 24, 0), RX_ALL_NV100);

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
}

size_t SoapySidekiq::mappedRxChannel(const size_t channel) const
{
    if (!rx_channel_alias_enabled)
    {
        return channel;
    }

    if (channel != DEFAULT_CHANNEL)
    {
        throw std::runtime_error("RX channel " + std::to_string(channel) +
                                 " is not available when using the channel alias");
    }

    return rx_channel_alias;
}

skiq_rx_hdl_t SoapySidekiq::rxHandleForChannel(const size_t channel) const
{
    const size_t mapped_channel = mappedRxChannel(channel);

    if (this->param.rf_param.num_rx_channels > 0)
    {
        if (mapped_channel >= this->param.rf_param.num_rx_channels)
        {
            throw std::runtime_error("RX channel " + std::to_string(channel) +
                                     " is not available on this Sidekiq card");
        }

        const skiq_rx_hdl_t hdl = this->param.rf_param.rx_handles[mapped_channel];
        if (hdl >= skiq_rx_hdl_end)
        {
            throw std::runtime_error("RX channel " + std::to_string(channel) +
                                     " maps to an invalid Sidekiq handle");
        }
        return hdl;
    }

    if (mapped_channel >= skiq_rx_hdl_end)
    {
        throw std::runtime_error("RX channel " + std::to_string(channel) +
                                 " is not a valid Sidekiq handle");
    }
    return static_cast<skiq_rx_hdl_t>(mapped_channel);
}

skiq_tx_hdl_t SoapySidekiq::txHandleForChannel(const size_t channel) const
{
    if (this->param.rf_param.num_tx_channels > 0)
    {
        if (channel >= this->param.rf_param.num_tx_channels)
        {
            throw std::runtime_error("TX channel " + std::to_string(channel) +
                                     " is not available on this Sidekiq card");
        }

        const skiq_tx_hdl_t hdl = this->param.rf_param.tx_handles[channel];
        if (hdl >= skiq_tx_hdl_end)
        {
            throw std::runtime_error("TX channel " + std::to_string(channel) +
                                     " maps to an invalid Sidekiq handle");
        }
        return hdl;
    }

    if (channel >= skiq_tx_hdl_end)
    {
        throw std::runtime_error("TX channel " + std::to_string(channel) +
                                 " is not a valid Sidekiq handle");
    }
    return static_cast<skiq_tx_hdl_t>(channel);
}

void SoapySidekiq::acquireSidekiqCard(const uint8_t requested_card,
                                      const skiq_xport_init_level_t level)
{
    int status = 0;

    if (requested_card >= SKIQ_MAX_NUM_CARDS)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Requested card %u is out of range",
                      requested_card);
        throw std::runtime_error("Requested Sidekiq card is out of range");
    }

    std::lock_guard<std::mutex> lock(sidekiq_init_mutex);

    if (!sidekiq_library_initialized)
    {
        status = skiq_init_without_cards();
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_init_without_cards failed, status %d",
                          status);
            throw std::runtime_error("skiq_init_without_cards failed");
        }
        sidekiq_library_initialized = true;
    }

    if (sidekiq_card_ref_count[requested_card] == 0)
    {
        const uint8_t cards[] = {requested_card};
        status = skiq_enable_cards(cards, 1, level);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_enable_cards failed (card %u), status %d",
                          requested_card, status);

            if (sidekiq_instance_count == 0)
            {
                const int exit_status = skiq_exit();
                if (exit_status != 0)
                {
                    SoapySDR_logf(SOAPY_SDR_WARNING,
                                  "skiq_exit failed after enable-card failure, status %d",
                                  exit_status);
                }
                sidekiq_library_initialized = false;
            }

            throw std::runtime_error("skiq_enable_cards failed");
        }
    }

    sidekiq_card_ref_count[requested_card]++;
    sidekiq_instance_count++;
}

void SoapySidekiq::releaseSidekiqCard(const uint8_t released_card)
{
    std::lock_guard<std::mutex> lock(sidekiq_init_mutex);

    if (!sidekiq_library_initialized)
    {
        return;
    }

    if (released_card < SKIQ_MAX_NUM_CARDS &&
        sidekiq_card_ref_count[released_card] > 0)
    {
        sidekiq_card_ref_count[released_card]--;
        if (sidekiq_card_ref_count[released_card] == 0)
        {
            const uint8_t cards[] = {released_card};
            const int status = skiq_disable_cards(cards, 1);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_WARNING,
                              "skiq_disable_cards failed (card %u), status %d",
                              released_card, status);
            }
        }
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_WARNING,
                      "release requested for untracked Sidekiq card %u",
                      released_card);
    }

    if (sidekiq_instance_count > 0)
    {
        sidekiq_instance_count--;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_WARNING,
                      "Sidekiq instance release requested with zero references");
    }

    if (sidekiq_instance_count == 0)
    {
        const int status = skiq_exit();
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "skiq_exit failed, status %d", status);
        }
        sidekiq_library_initialized = false;
    }
}

// Constructor
SoapySidekiq::SoapySidekiq(const SoapySDR::Kwargs &args)
{
    int status = 0;
    uint8_t channels = 0;
    skiq_iq_order_t iq_order;
    int i;

    /* Register our own logging function before initializing the library */
    skiq_register_logging( logging_handler );
   
    SoapySDR_logf(SOAPY_SDR_TRACE, "in constructor", card);

    /* We need to set some default parameters in case the user does not */


    rxUseShort  = true;
    txUseShort  = true;
    iq_swap   = true;
    counter   = false;
    debug_ctr = 0;
    card      = 0;
    rx_block_size_in_words = 0;
    rx_block_size_in_bytes = 0;
    rx_payload_size_in_words = 1018;
    rx_payload_size_in_bytes = 0;
    rfTimeSource = true;
    timetype = "rf_timestamp";
    complete_count = 0;

    rx_running = false;
    rx_channel_alias_enabled = false;
    rx_channel_alias = DEFAULT_CHANNEL;

    if (args.count("card") != 0)
    {
        try
        {
            card = std::stoi(args.at("card"));
        }
        catch (const std::invalid_argument &)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "Requested card not found");
            throw std::runtime_error("");
        }
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "No cards found");
        throw std::runtime_error("");
    }

    const auto channel_arg = args.count("rx_channel") != 0
        ? args.find("rx_channel")
        : (args.count("channel") != 0 ? args.find("channel") : args.end());
    if (channel_arg != args.end())
    {
        try
        {
            rx_channel_alias = std::stoul(channel_arg->second);
            rx_channel_alias_enabled = true;
        }
        catch (const std::invalid_argument &)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "Requested RX channel is invalid");
            throw std::runtime_error("");
        }
        catch (const std::out_of_range &)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "Requested RX channel is out of range");
            throw std::runtime_error("");
        }
    }

    if (args.count("tx_block_size") != 0)
    {
        current_tx_block_size = std::stoi(args.at("tx_block_size"));
    }
    else
    {
        current_tx_block_size = DEFAULT_TX_BUFFER_LENGTH;
    }
    SoapySDR_logf(SOAPY_SDR_INFO, "TX block size set to %u", current_tx_block_size);

    rx_hdl = skiq_rx_hdl_A1;
    tx_hdl = skiq_tx_hdl_A1;

    skiq_xport_init_level_t level = skiq_xport_init_level_full;

    SoapySDR_logf(SOAPY_SDR_INFO, "Sidekiq opening card %u", card);

    acquireSidekiqCard(card, level);
    sidekiq_card_acquired = true;

    struct CardReleaseGuard
    {
        SoapySidekiq *device;
        bool active;

        ~CardReleaseGuard()
        {
            if (active)
            {
                device->releaseSidekiqCard(device->card);
                device->sidekiq_card_acquired = false;
            }
        }
    } card_release_guard{this, true};

    char *serial_str = nullptr;
    status = skiq_read_serial_string(card, &serial_str);
    if (status == 0 && serial_str != nullptr)
    {
        serial = serial_str;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_WARNING,
                      "skiq_read_serial_string failed, card %u, status %d",
                      card, status);
        serial = "";
    }

    status = skiq_write_chan_mode(card, skiq_chan_mode_single);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_chan_mode failed, card %u, status %d",
                      card, status);
        throw std::runtime_error("");
    }
    SoapySDR_logf(SOAPY_SDR_TRACE, "channel mode set to single");

    /* set default sample_rate and bandwidth */
    this->rx_sample_rate = DEFAULT_SAMPLE_RATE;
    this->tx_sample_rate = DEFAULT_SAMPLE_RATE;

    this->rx_bandwidth = DEFAULT_BANDWIDTH;
    this->tx_bandwidth = DEFAULT_BANDWIDTH;

    /* set default frequency */
    this->rx_center_frequency = DEFAULT_FREQUENCY;
    for (int h = 0; h < skiq_rx_hdl_end; h++)
    {
        this->rx_center_frequency_by_handle[h] = DEFAULT_FREQUENCY;
    }

    this->tx_center_frequency = DEFAULT_FREQUENCY;

    if (args.count("clock_source") > 0) 
    {
        setClockSource(args.at("clock_source"));
    }

    if (args.count("time_source") > 0) 
    {
        setTimeSource(args.at("time_source"));
    }


    status = skiq_read_parameters(card, &this->param);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_parameters failed, card %u, status %d",
                      card, status);
        throw std::runtime_error("");
    }

    part = param.card_param.part_type;
    part_str = skiq_part_string(part);

    if (partRequiresExactBuiltInSampleRate(part) &&
        rx_sample_rate == DEFAULT_SAMPLE_RATE &&
        tx_sample_rate == DEFAULT_SAMPLE_RATE &&
        rx_bandwidth == DEFAULT_BANDWIDTH &&
        tx_bandwidth == DEFAULT_BANDWIDTH)
    {
        rx_bandwidth = static_cast<uint32_t>(DEFAULT_SAMPLE_RATE * 0.80);
        tx_bandwidth = rx_bandwidth;
        SoapySDR_logf(SOAPY_SDR_INFO,
                      "using NV100/NVM2-safe default bandwidth %u Hz",
                      rx_bandwidth);
    }

    SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, part type is %s", card, part_str.c_str());

    /* set iq order to iq instead of qi */
    if (iq_swap == true)
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, setting iq mode to I then Q", this->card);
        iq_order = skiq_iq_order_iq;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, setting iq mode to Q then I", this->card);
        iq_order = skiq_iq_order_qi;
    }

    status = skiq_write_iq_order_mode(card, iq_order);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_iq_order_mode failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }

    status = skiq_read_num_rx_chans(card, &channels);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_num_rx_chans failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }
    num_rx_channels = channels;

    if (rx_channel_alias_enabled)
    {
        if (rx_channel_alias >= num_rx_channels)
        {
            throw std::runtime_error("Requested RX channel " +
                                     std::to_string(rx_channel_alias) +
                                     " is not available on this Sidekiq card");
        }

        const skiq_rx_hdl_t alias_handle = rxHandleForChannel(DEFAULT_CHANNEL);
        SoapySDR_logf(SOAPY_SDR_INFO,
                      "RX channel alias enabled: application channel 0 maps to Sidekiq RX channel %zu (%s)",
                      rx_channel_alias, rxHandleName(alias_handle));
    }

    status = skiq_read_num_tx_chans(card, &channels);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_num_tx_chans failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }
    num_tx_channels = channels;

    if (num_rx_channels > 0)
    {
        setFrequency(SOAPY_SDR_RX,
                     DEFAULT_CHANNEL,
                     static_cast<double>(this->rx_center_frequency));
    }

    if (num_tx_channels > 0)
    {
        setFrequency(SOAPY_SDR_TX,
                     DEFAULT_CHANNEL,
                     static_cast<double>(this->tx_center_frequency));
    }

    uint8_t tmp_resolution = 0;

    /* Every card can have a different iq resolution.  */
    status = skiq_read_rx_iq_resolution(card, &tmp_resolution);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_iq_resolution failed, "
                      "card: %u status: %d",
                      card, status);
        throw std::runtime_error("");
    }

    this->resolution = tmp_resolution;
    this->maxValue = (double) ((1 << (tmp_resolution-1))-1);

    SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, card resolution: %u bits, max ADC value: %u",
                  card, this->resolution, (uint32_t) this->maxValue);

    // allocate for # blocks
    p_tx_status = static_cast<int32_t*>(calloc(DEFAULT_NUM_BUFFERS, sizeof(*p_tx_status)));
    if (p_tx_status == NULL)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "failed to allocate memory for TX status");
        throw std::runtime_error("");
    }

    for (i = 0; i < DEFAULT_NUM_BUFFERS; i++)
    {
        p_tx_status[i] = 0;
    }

    // register the transmit complete callback
    status = skiq_register_tx_complete_callback(card,
                                        &SoapySidekiq::static_tx_complete_callback);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_register_tx_complete_callback failed, "
                      "card: %u status: %d",
                      card, status);
        throw std::runtime_error("");
    }
    pthread_mutex_init(&space_avail_mutex, nullptr);
    pthread_cond_init(&space_avail_cond, nullptr);


    // register the transmit enabled callback
    status = skiq_register_tx_enabled_callback(card,
                                        &SoapySidekiq::static_tx_enabled_callback);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_register_tx_enabled_callback failed, "
                      "card: %u status: %d",
                      card, status);
        throw std::runtime_error("");
    }

    pthread_mutex_init(&tx_enabled_mutex, nullptr);
    pthread_cond_init(&tx_enabled_cond, nullptr);

    card_release_guard.active = false;

    SoapySDR_logf(SOAPY_SDR_TRACE, "leaving constructor", card);
}

// Destructor
SoapySidekiq::~SoapySidekiq(void)
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "In destructor", card);

    if (NULL != p_tx_status)
    {
        free(p_tx_status);
        p_tx_status = NULL;
    }

    if (sidekiq_card_acquired)
    {
        releaseSidekiqCard(card);
        sidekiq_card_acquired = false;
    }
}

/*******************************************************************
 * Identification API
 ******************************************************************/

std::string SoapySidekiq::getDriverKey(void) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getDriverKey");
    return "Sidekiq";
}
std::string SoapySidekiq::getHardwareKey(void) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getHardwareKey");
    return part_str;
}



SoapySDR::Kwargs SoapySidekiq::getHardwareInfo(void) const
{
    //  key/value pairs for any useful information
    //  this also gets printed in --probe
    SoapySDR::Kwargs args;
    SoapySDR_logf(SOAPY_SDR_TRACE, "getHardwareInfo");

    args["origin"] = "https://github.com/epiqsolutions/SoapySidekiq";
    args["card_type"] = part_str;
    args["card"]   = std::to_string(card);
    args["serial"]   = serial;
    args["rx_channels"] = std::to_string(getNumChannels(SOAPY_SDR_RX));
    args["tx_channels"]   = std::to_string(num_tx_channels);
    if (rx_channel_alias_enabled)
    {
        args["sidekiq_rx_channel"] = std::to_string(rx_channel_alias);
    }

    return args;
}

/*******************************************************************
 * Channels API
 ******************************************************************/

size_t SoapySidekiq::getNumChannels(const int dir) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getNumChannels");

    if (dir == SOAPY_SDR_RX)
    {
        if (rx_channel_alias_enabled)
        {
            return 1;
        }
        return num_rx_channels;
    }
    else if (dir == SOAPY_SDR_TX)
    {
        return num_tx_channels;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", dir);
        throw std::runtime_error("");
    }

    return -1;
}

SoapySDR::Kwargs SoapySidekiq::getChannelInfo(const int direction,
                                              const size_t channel) const
{
    SoapySDR::Kwargs info;

    SoapySDR_logf(SOAPY_SDR_TRACE, "getChannelInfo");

    info["card"] = std::to_string(card);
    info["serial"] = serial;
    info["card_type"] = part_str;

    if (direction == SOAPY_SDR_RX)
    {
        const size_t mapped_channel = mappedRxChannel(channel);
        const skiq_rx_hdl_t handle = rxHandleForChannel(channel);
        info["label"] = "RX " + std::string(rxHandleName(handle));
        info["sidekiq_handle"] = rxHandleName(handle);
        info["sidekiq_handle_id"] = std::to_string(static_cast<int>(handle));
        info["sidekiq_channel"] = std::to_string(mapped_channel);
        info["rf_chain"] = rxHandleMayShareLo(handle) ? "secondary" : "primary";
    }
    else if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t handle = txHandleForChannel(channel);
        info["label"] = "TX " + std::string(txHandleName(handle));
        info["sidekiq_handle"] = txHandleName(handle);
        info["sidekiq_handle_id"] = std::to_string(static_cast<int>(handle));
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return info;
}

/*******************************************************************
 * Antenna API
 ******************************************************************/

std::vector<std::string> SoapySidekiq::listAntennas(const int direction, const size_t channel) const {
    std::vector<std::string> antennas;
    int status = 0;
    uint8_t num_fixed_ports = 0;
    uint8_t num_trx_ports = 0;
    skiq_rf_port_t fixed_ports[skiq_rf_port_max] = {};
    skiq_rf_port_t trx_ports[skiq_rf_port_max] = {};

    SoapySDR_logf(SOAPY_SDR_TRACE, "listAntennas");

    if (direction == SOAPY_SDR_RX)
    {
        const skiq_rx_hdl_t hdl = rxHandleForChannel(channel);
        status = skiq_read_rx_rf_ports_avail_for_hdl(card,
                                                     hdl,
                                                     &num_fixed_ports,
                                                     fixed_ports,
                                                     &num_trx_ports,
                                                     trx_ports);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "skiq_read_rx_rf_ports_avail_for_hdl failed "
                          "(card %u, handle %s), status %d",
                          card, rxHandleName(hdl), status);

            for (uint8_t index = 0;
                 index < this->param.rx_param[hdl].num_fixed_rf_ports;
                 index++)
            {
                appendRfPortName(antennas,
                                 this->param.rx_param[hdl].fixed_rf_ports[index]);
            }
            for (uint8_t index = 0;
                 index < this->param.rx_param[hdl].num_trx_rf_ports;
                 index++)
            {
                appendRfPortName(antennas,
                                 this->param.rx_param[hdl].trx_rf_ports[index]);
            }
        }
        else
        {
            for (uint8_t index = 0; index < num_fixed_ports; index++)
            {
                appendRfPortName(antennas, fixed_ports[index]);
            }
            for (uint8_t index = 0; index < num_trx_ports; index++)
            {
                appendRfPortName(antennas, trx_ports[index]);
            }
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t hdl = txHandleForChannel(channel);
        status = skiq_read_tx_rf_ports_avail_for_hdl(card,
                                                     hdl,
                                                     &num_fixed_ports,
                                                     fixed_ports,
                                                     &num_trx_ports,
                                                     trx_ports);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "skiq_read_tx_rf_ports_avail_for_hdl failed "
                          "(card %u, handle %s), status %d",
                          card, txHandleName(hdl), status);

            for (uint8_t index = 0;
                 index < this->param.tx_param[hdl].num_fixed_rf_ports;
                 index++)
            {
                appendRfPortName(antennas,
                                 this->param.tx_param[hdl].fixed_rf_ports[index]);
            }
            for (uint8_t index = 0;
                 index < this->param.tx_param[hdl].num_trx_rf_ports;
                 index++)
            {
                appendRfPortName(antennas,
                                 this->param.tx_param[hdl].trx_rf_ports[index]);
            }
        }
        else
        {
            for (uint8_t index = 0; index < num_fixed_ports; index++)
            {
                appendRfPortName(antennas, fixed_ports[index]);
            }
            for (uint8_t index = 0; index < num_trx_ports; index++)
            {
                appendRfPortName(antennas, trx_ports[index]);
            }
        }
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    if (antennas.empty())
    {
        antennas.push_back("NONE");
    }

    return antennas;
}

void SoapySidekiq::setAntenna(const int direction,
                              const size_t channel,
                              const std::string &name)
{
    int status = 0;
    uint8_t num_fixed_ports = 0;
    uint8_t num_trx_ports = 0;
    skiq_rf_port_t fixed_ports[skiq_rf_port_max] = {};
    skiq_rf_port_t trx_ports[skiq_rf_port_max] = {};
    bool transmit = false;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setAntenna");

    if (name == "NONE")
    {
        throw std::runtime_error("NONE is not a configurable Sidekiq RF port");
    }

    if (direction == SOAPY_SDR_RX)
    {
        const skiq_rx_hdl_t hdl = rxHandleForChannel(channel);
        skiq_rf_port_t current_port = skiq_rf_port_unknown;

        status = skiq_read_rx_rf_ports_avail_for_hdl(card,
                                                     hdl,
                                                     &num_fixed_ports,
                                                     fixed_ports,
                                                     &num_trx_ports,
                                                     trx_ports);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_rf_ports_avail_for_hdl failed "
                          "(card %u, handle %s), status %d",
                          card, rxHandleName(hdl), status);
            throw std::runtime_error("failed to read RX RF ports");
        }

        const skiq_rf_port_t requested_port =
            rfPortFromAntennaName(name,
                                  fixed_ports,
                                  num_fixed_ports,
                                  trx_ports,
                                  num_trx_ports);
        if (requested_port == skiq_rf_port_unknown)
        {
            throw std::runtime_error("RX antenna " + name +
                                     " is not available on this channel");
        }

        status = skiq_read_rx_rf_port_for_hdl(card, hdl, &current_port);
        if (status == 0 && current_port == requested_port)
        {
            SoapySDR_logf(SOAPY_SDR_INFO,
                          "RX channel %zu handle %s already uses RF port %s",
                          channel, rxHandleName(hdl), rfPortName(requested_port).c_str());
            return;
        }
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "skiq_read_rx_rf_port_for_hdl failed "
                          "(card %u, handle %s), status %d",
                          card, rxHandleName(hdl), status);
        }

        const bool use_trx =
            findPortInList(requested_port, trx_ports, num_trx_ports);
        const bool use_fixed =
            findPortInList(requested_port, fixed_ports, num_fixed_ports);
        if (!use_fixed && !use_trx)
        {
            throw std::runtime_error("RX antenna " + name +
                                     " is not available on this channel");
        }

        status = skiq_write_rf_port_config(
            card,
            use_trx ? skiq_rf_port_config_trx : skiq_rf_port_config_fixed);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_rf_port_config failed "
                          "(card %u, config %d), status %d",
                          card,
                          use_trx ? skiq_rf_port_config_trx : skiq_rf_port_config_fixed,
                          status);
            throw std::runtime_error("failed to configure RF port mode");
        }

        if (use_trx)
        {
            status = skiq_write_rf_port_operation(card, transmit);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_write_rf_port_operation failed "
                              "(card %u, receive), status %d",
                              card, status);
                throw std::runtime_error("failed to configure TRX port operation");
            }
        }

        status = skiq_write_rx_rf_port_for_hdl(card, hdl, requested_port);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_rx_rf_port_for_hdl failed "
                          "(card %u, handle %s, port %s), status %d",
                          card,
                          rxHandleName(hdl),
                          rfPortName(requested_port).c_str(),
                          status);
            throw std::runtime_error("failed to configure RX antenna");
        }

        SoapySDR_logf(SOAPY_SDR_INFO,
                      "RX channel %zu handle %s uses RF port %s",
                      channel, rxHandleName(hdl), rfPortName(requested_port).c_str());
    }
    else if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t hdl = txHandleForChannel(channel);
        skiq_rf_port_t current_port = skiq_rf_port_unknown;
        transmit = true;

        status = skiq_read_tx_rf_ports_avail_for_hdl(card,
                                                     hdl,
                                                     &num_fixed_ports,
                                                     fixed_ports,
                                                     &num_trx_ports,
                                                     trx_ports);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_rf_ports_avail_for_hdl failed "
                          "(card %u, handle %s), status %d",
                          card, txHandleName(hdl), status);
            throw std::runtime_error("failed to read TX RF ports");
        }

        const skiq_rf_port_t requested_port =
            rfPortFromAntennaName(name,
                                  fixed_ports,
                                  num_fixed_ports,
                                  trx_ports,
                                  num_trx_ports);
        if (requested_port == skiq_rf_port_unknown)
        {
            throw std::runtime_error("TX antenna " + name +
                                     " is not available on this channel");
        }

        status = skiq_read_tx_rf_port_for_hdl(card, hdl, &current_port);
        if (status == 0 && current_port == requested_port)
        {
            SoapySDR_logf(SOAPY_SDR_INFO,
                          "TX channel %zu handle %s already uses RF port %s",
                          channel, txHandleName(hdl), rfPortName(requested_port).c_str());
            return;
        }
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "skiq_read_tx_rf_port_for_hdl failed "
                          "(card %u, handle %s), status %d",
                          card, txHandleName(hdl), status);
        }

        const bool use_trx =
            findPortInList(requested_port, trx_ports, num_trx_ports);
        const bool use_fixed =
            findPortInList(requested_port, fixed_ports, num_fixed_ports);
        if (!use_fixed && !use_trx)
        {
            throw std::runtime_error("TX antenna " + name +
                                     " is not available on this channel");
        }

        status = skiq_write_rf_port_config(
            card,
            use_trx ? skiq_rf_port_config_trx : skiq_rf_port_config_fixed);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_rf_port_config failed "
                          "(card %u, config %d), status %d",
                          card,
                          use_trx ? skiq_rf_port_config_trx : skiq_rf_port_config_fixed,
                          status);
            throw std::runtime_error("failed to configure RF port mode");
        }

        if (use_trx)
        {
            status = skiq_write_rf_port_operation(card, transmit);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_write_rf_port_operation failed "
                              "(card %u, transmit), status %d",
                              card, status);
                throw std::runtime_error("failed to configure TRX port operation");
            }
        }

        status = skiq_write_tx_rf_port_for_hdl(card, hdl, requested_port);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_tx_rf_port_for_hdl failed "
                          "(card %u, handle %s, port %s), status %d",
                          card,
                          txHandleName(hdl),
                          rfPortName(requested_port).c_str(),
                          status);
            throw std::runtime_error("failed to configure TX antenna");
        }

        SoapySDR_logf(SOAPY_SDR_INFO,
                      "TX channel %zu handle %s uses RF port %s",
                      channel, txHandleName(hdl), rfPortName(requested_port).c_str());
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }
}

std::string SoapySidekiq::getAntenna(const int direction,
                                     const size_t channel) const
{
    int status = 0;
    skiq_rf_port_t port = skiq_rf_port_unknown;
    const auto fallbackAntenna = [this, direction, channel]() -> std::string
    {
        const std::vector<std::string> antennas = listAntennas(direction, channel);
        if (!antennas.empty())
        {
            return antennas.front();
        }

        return "NONE";
    };

    SoapySDR_logf(SOAPY_SDR_TRACE, "getAntenna");

    if (direction == SOAPY_SDR_RX)
    {
        const skiq_rx_hdl_t hdl = rxHandleForChannel(channel);
        status = skiq_read_rx_rf_port_for_hdl(card, hdl, &port);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_rf_port_for_hdl failed "
                          "(card %u, handle %s), status %d",
                          card, rxHandleName(hdl), status);
            return fallbackAntenna();
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t hdl = txHandleForChannel(channel);
        status = skiq_read_tx_rf_port_for_hdl(card, hdl, &port);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_rf_port_for_hdl failed "
                          "(card %u, handle %s), status %d",
                          card, txHandleName(hdl), status);
            return fallbackAntenna();
        }
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    const std::string name = rfPortName(port);
    if (name == "NONE")
    {
        return fallbackAntenna();
    }

    return name;
}

/*******************************************************************
 * Frontend corrections API
 ******************************************************************/

bool SoapySidekiq::hasDCOffsetMode(const int direction,
                                   const size_t channel) const
{
    int status = 0;
    uint32_t mask = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "hasDCOffsetMode");

    if (direction == SOAPY_SDR_RX)
    {
        const skiq_rx_hdl_t hdl = rxHandleForChannel(channel);
        status = skiq_read_rx_cal_types_avail(card, hdl, &mask);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_rx_cal_types_avail failed, "
                          "(card %u, handle %u, mask %d), status %d",
                          card, hdl, mask, status);
            throw std::runtime_error("");
        }

        if ((mask & skiq_rx_cal_type_dc_offset) == skiq_rx_cal_type_dc_offset)
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, handle %u, has DC Offest correction, " 
                          "mask is: %d",
                          card, hdl, mask);
            return true;
        }
        else
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, handle %u, does not have DC Offest correction, " 
                          "mask is: %d",
                          card, hdl, mask);
            return false;
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        // Tx has quadcal only
        SoapySDR_logf(SOAPY_SDR_WARNING, "TX does not have DC offset mode");
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid Direction %d", direction);
        throw std::runtime_error("");
    }

    return false;
}

void SoapySidekiq::setDCOffsetMode(const int direction,
                                   const size_t channel,
                                   const bool automatic)
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "setDCOffsetMode");

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = rxHandleForChannel(channel);

        skiq_rx_cal_mode_t cal_mode = skiq_rx_cal_mode_auto;

        // if automatic is false, then we need to set the mode to manual
        // otherwise set the mode to automatic
        if (automatic == false)
        {
            cal_mode = skiq_rx_cal_mode_manual;
        }

        status = skiq_write_rx_cal_mode(card, this->rx_hdl, cal_mode);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_rx_cal_mode failed "
                          "(card %u, cal_mode %d), status %d",
                          card, cal_mode, status);
            throw std::runtime_error("");
        }

        SoapySDR_logf(SOAPY_SDR_INFO, "channel: %u, setting DC offset correction to %s mode",
                      channel, (bool)cal_mode ? "automatic" : "manual");
    }
    else if (direction == SOAPY_SDR_TX)
    {
        // Tx has quadcal only
        SoapySDR_logf(SOAPY_SDR_WARNING, "TX does not have DC offset mode");
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }
}

bool SoapySidekiq::getDCOffsetMode(const int direction,
                                   const size_t channel) const
{
    skiq_rx_cal_mode_t cal_mode;
    int  status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "getDCOffsetMode");

    if (direction == SOAPY_SDR_RX)
    {
        const skiq_rx_hdl_t hdl = rxHandleForChannel(channel);
        status = skiq_read_rx_cal_mode(card, hdl, &cal_mode);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_cal_mode failure "
                          "(card %u, mode %d), status %d",
                          card, cal_mode, status);
        throw std::runtime_error("");
        }

        SoapySDR_logf(SOAPY_SDR_INFO, "Channel %d, DC offest mode is %s",
                      channel, (bool)cal_mode ? "automatic" : "manual");
    }
    else if (direction == SOAPY_SDR_TX)
    {
        // Tx has quadcal only
        SoapySDR_logf(SOAPY_SDR_WARNING, "TX does not have DC offset mode");
        return false;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return (bool)cal_mode;
}

/*******************************************************************
 * Gain API
 ******************************************************************/

std::vector<std::string> SoapySidekiq::listGains(const int direction,
                                                 const size_t channel) const
{
    (void)channel;
    std::vector<std::string> results;
    SoapySDR_logf(SOAPY_SDR_TRACE, "listGains");

    if (direction == SOAPY_SDR_RX)
    {
        results.push_back("LNA");
    }
    else if (direction == SOAPY_SDR_TX)
    {
        results.push_back("attenuation");
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return results;
}

bool SoapySidekiq::hasGainMode(const int direction, const size_t channel) const
{
    (void)channel;
    SoapySDR_logf(SOAPY_SDR_TRACE, "hasGainMode");

    if (direction == SOAPY_SDR_RX)
    {
        return true;
    }
    if (direction == SOAPY_SDR_TX)
    {
        return false;
    }

    SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
    throw std::runtime_error("");
}

void SoapySidekiq::setGainMode(const int direction, const size_t channel,
                               const bool automatic)
{
    int status = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setGainMode");

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = rxHandleForChannel(channel);

        skiq_rx_gain_t mode =
            automatic ? skiq_rx_gain_auto : skiq_rx_gain_manual;

        status = skiq_write_rx_gain_mode(card, this->rx_hdl, mode);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_rx_gain_mode failed "
                          "(card %u, channel %d, mode %d), status %d",
                          card, channel, mode, status);
            throw std::runtime_error("");
        }

        SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, handle: %u, setting RX gain mode: %s",
                      card, this->rx_hdl, automatic ? "skiq_rx_gain_auto" :
                      "skiq_rx_gain_manual");
    }
    else if (direction == SOAPY_SDR_TX)
    {
        // Tx has no mode
        SoapySDR_logf(SOAPY_SDR_WARNING, "TX does not have an attenuation mode");
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }
}

bool SoapySidekiq::getGainMode(const int direction, const size_t channel) const
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "getGainMode");

    if (direction == SOAPY_SDR_RX)
    {
        skiq_rx_gain_t p_gain_mode;
        const skiq_rx_hdl_t hdl = rxHandleForChannel(channel);

        status = skiq_read_rx_gain_mode(card, hdl, &p_gain_mode);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_gain_mode failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, handle: %u, RX gain mode: is %s",
                      card, hdl, p_gain_mode ? "skiq_rx_gain_auto" :
                      "skiq_rx_gain_manual");

        return p_gain_mode;
    }
    else if (direction == SOAPY_SDR_TX)
    {
        // Tx has no mode
        SoapySDR_logf(SOAPY_SDR_WARNING, "TX does not have an attenuation mode");
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return false;
}

void SoapySidekiq::setGain(const int direction, 
                           const size_t channel, 
                           const std::string &name, 
                           const double value)
{
    if (direction == SOAPY_SDR_RX)
    {
        if (!isRxGainName(name))
        {
            throw std::runtime_error("unsupported RX gain element '" + name + "'");
        }

        setGain(direction, channel, value);
        return;
    }

    if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t hdl = txHandleForChannel(channel);

        if (isTxOutputGainName(name))
        {
            setGain(direction, channel, value);
            return;
        }

        if (!isTxAttenuationName(name))
        {
            throw std::runtime_error("unsupported TX gain element '" + name + "'");
        }

        const SoapySDR::Range range = txAttenuationRangeDb(this->param, hdl);
        if (!rangeContains(range, value))
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "card: %u, invalid requested TX attenuation: %.2f dB, "
                          "acceptable range: %.2f - %.2f dB. No attenuation configured.",
                          card,
                          value,
                          range.minimum(),
                          range.maximum());
            return;
        }

        const uint16_t attenuation_index =
            txAttenuationIndexFromAttenuationDb(this->param, hdl, value);
        uint16_t current_attenuation_index = 0;
        int status = skiq_read_tx_attenuation(card,
                                              hdl,
                                              &current_attenuation_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_attenuation failed "
                          "(card %u, handle %u), status %d",
                          card,
                          hdl,
                          status);
            throw std::runtime_error("");
        }

        if (current_attenuation_index == attenuation_index)
        {
            SoapySDR_logf(SOAPY_SDR_DEBUG,
                          "card: %u, handle: %u, TX attenuation already %.2f dB "
                          "(attenuation index: %u)",
                          card,
                          hdl,
                          txAttenuationDbFromAttenuationIndex(this->param,
                                                              hdl,
                                                              attenuation_index),
                          attenuation_index);
            return;
        }

        status = skiq_write_tx_attenuation(card, hdl, attenuation_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_tx_attenuation failed "
                          "(card %u, handle %u, attenuation_index %u), status %d",
                          card,
                          hdl,
                          attenuation_index,
                          status);
            throw std::runtime_error("");
        }

        SoapySDR_logf(SOAPY_SDR_INFO,
                      "card: %u, handle: %u, Setting TX attenuation: %.2f dB "
                      "(attenuation index: %u)",
                      card,
                      hdl,
                      txAttenuationDbFromAttenuationIndex(this->param,
                                                          hdl,
                                                          attenuation_index),
                      attenuation_index);
        return;
    }

    SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
    throw std::runtime_error("");
}

void SoapySidekiq::setGain(const int direction, 
                           const size_t channel, 
                           const double value)
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "setGain called with direction %d, channel %zu, value %.1f", direction, channel, value);

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = rxHandleForChannel(channel);

        // 1. Read current gain mode
        skiq_rx_gain_t gain_mode;
        status = skiq_read_rx_gain_mode(card, this->rx_hdl, &gain_mode);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_read_rx_gain_mode failed (card %u, channel %zu), status %d",
                card, channel, status);
            throw std::runtime_error("");
        }

        // 2. Force manual mode if currently in auto
        if (gain_mode == skiq_rx_gain_auto)
        {
            SoapySDR_logf(SOAPY_SDR_INFO, 
                "Gain mode was auto, switching to manual for explicit gain setting.");

            status = skiq_write_rx_gain_mode(card, this->rx_hdl, skiq_rx_gain_manual);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                    "skiq_write_rx_gain_mode failed (card %u, channel %zu), status %d",
                    card, channel, status);
                throw std::runtime_error("");
            }
        }

        if (!std::isfinite(value))
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "card: %u, invalid requested RX gain: %.2f dB. "
                          "No gain configured.",
                          card,
                          value);
            return;
        }

        const RxGainIndexRange gain_range =
            readRxGainIndexRange(card, this->rx_hdl);
        const SoapySDR::Range gain_db_range =
            rxGainRangeDb(part, this->rx_hdl, gain_range);
        if (!rangeContains(gain_db_range, value))
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "card: %u, requested RX gain %.2f dB is outside "
                          "the supported range %.2f - %.2f dB; clamping",
                          card,
                          value,
                          gain_db_range.minimum(),
                          gain_db_range.maximum());
        }

        const uint8_t gain_index =
            rxGainIndexFromDb(part, this->rx_hdl, gain_range, value);
        uint8_t current_gain_index = 0;
        status = skiq_read_rx_gain(card, this->rx_hdl, &current_gain_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_gain failed "
                          "(card %u, handle %u), status %d",
                          card,
                          this->rx_hdl,
                          status);
            throw std::runtime_error("");
        }

        if (current_gain_index == gain_index)
        {
            SoapySDR_logf(SOAPY_SDR_DEBUG,
                          "card: %u, handle: %u, RX gain already %.2f dB "
                          "(gain index %u, index range [%u-%u])",
                          card,
                          this->rx_hdl,
                          rxGainDbFromIndex(part,
                                            this->rx_hdl,
                                            gain_range,
                                            gain_index),
                          gain_index,
                          gain_range.minimum,
                          gain_range.maximum);
            return;
        }

        SoapySDR_logf(SOAPY_SDR_INFO,
            "card: %u, handle: %u, Set RX gain: requested %.2f dB, "
            "actual %.2f dB, gain index %u, index range [%u-%u]",
            card,
            this->rx_hdl,
            value,
            rxGainDbFromIndex(part, this->rx_hdl, gain_range, gain_index),
            gain_index,
            gain_range.minimum,
            gain_range.maximum);

        status = skiq_write_rx_gain(card, this->rx_hdl, gain_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_write_rx_gain failed (card %u, gain_index %u), status %d",
                card, gain_index, status);
            throw std::runtime_error("");
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t hdl = txHandleForChannel(channel);
        const SoapySDR::Range range = txOutputGainRangeDb(this->param, hdl);
        if (!rangeContains(range, value))
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "card: %u, invalid requested TX output gain: %.2f dB, "
                          "acceptable range: %.2f - %.2f dB. No attenuation configured.",
                          card,
                          value,
                          range.minimum(),
                          range.maximum());
            return;
        }

        const uint16_t attenuation_index =
            txAttenuationIndexFromOutputGainDb(this->param, hdl, value);
        uint16_t current_attenuation_index = 0;
        status = skiq_read_tx_attenuation(card,
                                          hdl,
                                          &current_attenuation_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_attenuation failed "
                          "(card %u, handle %u), status %d",
                          card,
                          hdl,
                          status);
            throw std::runtime_error("");
        }

        if (current_attenuation_index == attenuation_index)
        {
            SoapySDR_logf(SOAPY_SDR_DEBUG,
                          "card: %u, handle: %u, TX output gain already %.2f dB "
                          "(actual attenuation %.2f dB, attenuation index: %u)",
                          card,
                          hdl,
                          txOutputGainDbFromAttenuationIndex(this->param,
                                                             hdl,
                                                             attenuation_index),
                          txAttenuationDbFromAttenuationIndex(this->param,
                                                              hdl,
                                                              attenuation_index),
                          attenuation_index);
            return;
        }

        status = skiq_write_tx_attenuation(card, hdl, attenuation_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_write_tx_attenuation failed, (card %u, index %u), status %d",
                card, attenuation_index, status);
            throw std::runtime_error("");
        }

        SoapySDR_logf(SOAPY_SDR_INFO,
                      "card: %u, handle: %u, Setting TX output gain: %.2f dB "
                      "(actual attenuation %.2f dB, attenuation index: %u)",
                      card,
                      hdl,
                      txOutputGainDbFromAttenuationIndex(this->param,
                                                         hdl,
                                                         attenuation_index),
                      txAttenuationDbFromAttenuationIndex(this->param,
                                                          hdl,
                                                          attenuation_index),
                      attenuation_index);
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }
}

double SoapySidekiq::getGain(const int direction, const size_t channel, const std::string &name) const
{
    if (direction == SOAPY_SDR_RX)
    {
        if (!isRxGainName(name))
        {
            throw std::runtime_error("unsupported RX gain element '" + name + "'");
        }
        return getGain(direction, channel);
    }

    if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t hdl = txHandleForChannel(channel);

        if (isTxOutputGainName(name))
        {
            return getGain(direction, channel);
        }

        if (!isTxAttenuationName(name))
        {
            throw std::runtime_error("unsupported TX gain element '" + name + "'");
        }

        uint16_t attenuation_index = 0;
        const int status = skiq_read_tx_attenuation(card, hdl, &attenuation_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_attenuation failed "
                          "(card %u, handle %u), status %d",
                          card,
                          hdl,
                          status);
            throw std::runtime_error("");
        }

        return txAttenuationDbFromAttenuationIndex(this->param,
                                                  hdl,
                                                  attenuation_index);
    }

    SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
    throw std::runtime_error("");
}

double SoapySidekiq::getGain(const int direction, const size_t channel) const
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "getGain, direction: %d channel: %zu", direction, channel);

    if (direction == SOAPY_SDR_RX)
    {
        uint8_t gain_index;
        const skiq_rx_hdl_t hdl = rxHandleForChannel(channel);
        status = skiq_read_rx_gain(card, hdl, &gain_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_read_rx_gain failed (card %u), rx_hdl %zu, status %d",
                card, channel, status);
            throw std::runtime_error("");
        }

        const RxGainIndexRange gain_range = readRxGainIndexRange(card, hdl);
        return rxGainDbFromIndex(part, hdl, gain_range, gain_index);
    }
    else if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t hdl = txHandleForChannel(channel);
        uint16_t attenuation_index = 0;

        status = skiq_read_tx_attenuation(card, 
                                          hdl,
                                          &attenuation_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_read_tx_attenuation failed (card %u), status %d",
                card, status);
            throw std::runtime_error("");
        }

        return txOutputGainDbFromAttenuationIndex(this->param,
                                                 hdl,
                                                 attenuation_index);
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return 0;
}

SoapySDR::Range SoapySidekiq::getGainRange(const int    direction,
                                           const size_t channel,
                                           const std::string & name) const
{
    SoapySDR_log(SOAPY_SDR_TRACE, "getGainRange with name");

    if (direction == SOAPY_SDR_RX)
    {
        if (!isRxGainName(name))
        {
            throw std::runtime_error("unsupported RX gain element '" + name + "'");
        }
        return getGainRange(direction, channel);
    }

    if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t hdl = txHandleForChannel(channel);
        if (isTxOutputGainName(name))
        {
            return getGainRange(direction, channel);
        }
        if (isTxAttenuationName(name))
        {
            return txAttenuationRangeDb(this->param, hdl);
        }

        throw std::runtime_error("unsupported TX gain element '" + name + "'");
    }

    SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
    throw std::runtime_error("");
}


SoapySDR::Range SoapySidekiq::getGainRange(const int    direction,
                                           const size_t channel) const
{
    SoapySDR_log(SOAPY_SDR_TRACE, "getGainRange");

    if (direction == SOAPY_SDR_RX)
    {
        const skiq_rx_hdl_t hdl = rxHandleForChannel(channel);
        const RxGainIndexRange gain_range = readRxGainIndexRange(card, hdl);
        return rxGainRangeDb(part, hdl, gain_range);
    }
    else if (direction == SOAPY_SDR_TX)
    {
        return txOutputGainRangeDb(this->param, txHandleForChannel(channel));
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return SoapySDR::Device::getGainRange(direction, channel);
}

/*******************************************************************
 * Frequency API
 ******************************************************************/

void SoapySidekiq::writeRxFrequency(const skiq_rx_hdl_t handle,
                                    const uint64_t frequency)
{
    const int status = skiq_write_rx_LO_freq(this->card, handle, frequency);
    if (status == -EDOM && rxHandleMayShareLo(handle))
    {
        SoapySDR_logf(SOAPY_SDR_WARNING,
                      "skiq_write_rx_LO_freq rejected secondary RX handle %u; "
                      "it may share the LO with its primary handle",
                      handle);
        return;
    }

    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_write_rx_LO_freq failed, (card %u, handle %u, "
                      "frequency %lu), status %d",
                      this->card, handle, frequency, status);
        throw std::runtime_error("");
    }
}

void SoapySidekiq::setFrequency(const int direction, const size_t channel,
                  const double frequency, const SoapySDR::Kwargs &args)
{
    int status = 0;
    const uint64_t requested_frequency = hzToUint64("frequency", frequency);

    SoapySDR_log(SOAPY_SDR_TRACE, "setFrequency");

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = rxHandleForChannel(channel);
        validateRangeValue("RX frequency",
                           static_cast<double>(requested_frequency),
                           getFrequencyRange(direction, channel).front());

        this->rx_center_frequency = requested_frequency;
        this->rx_center_frequency_by_handle[this->rx_hdl] = this->rx_center_frequency;

        SoapySDR_logf(SOAPY_SDR_INFO, "Setting rx center freq: %lu on handle %u",
                      rx_center_frequency, this->rx_hdl);

        writeRxFrequency(this->rx_hdl, this->rx_center_frequency);
    }
    else if (direction == SOAPY_SDR_TX)
    {
        this->tx_hdl = txHandleForChannel(channel);
        validateRangeValue("TX frequency",
                           static_cast<double>(requested_frequency),
                           getFrequencyRange(direction, channel).front());

        this->tx_center_frequency = requested_frequency;

        SoapySDR_logf(SOAPY_SDR_INFO, "Setting tx center freq: %lu",
                      tx_center_frequency);

        status = skiq_write_tx_LO_freq(this->card, this->tx_hdl, tx_center_frequency);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_tx_LO_freq failed, (card %u, frequency "
                          "%lu), status %d",
                          this->card, tx_center_frequency, status);
            throw std::runtime_error("");
        }
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }
}

double SoapySidekiq::getFrequency(const int direction, const size_t channel) const
{
    int status = 0;

    SoapySDR_log(SOAPY_SDR_TRACE, "getFrequency");

    if (direction == SOAPY_SDR_RX)
    {
        uint64_t freq;
        double   tuned_freq;
        status = skiq_read_rx_LO_freq(card,
                                      rxHandleForChannel(channel),
                                      &freq,
                                      &tuned_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_LO_freq (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        return static_cast<double>(freq);
    }
    else if (direction == SOAPY_SDR_TX)
    {
        uint64_t freq;
        double   tuned_freq;
        status = skiq_read_tx_LO_freq(card,
                                      txHandleForChannel(channel),
                                      &freq,
                                      &tuned_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_LO_freq (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        return static_cast<double>(freq);
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return 0;
}

SoapySDR::RangeList SoapySidekiq::getFrequencyRange(const int direction,
                                                    const size_t channel) const
{
    SoapySDR::RangeList results;

    SoapySDR_log(SOAPY_SDR_TRACE, "getFrequencyRange");

    uint64_t max;
    uint64_t min;
    int      status = 0;

    if (direction == SOAPY_SDR_RX)
    {
        status = skiq_read_rx_LO_freq_range(card, &max, &min);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_LO_freq_range failed,(card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        results.push_back(SoapySDR::Range(min, max));
    }
    else if (direction == SOAPY_SDR_TX)
    {
        status = skiq_read_tx_LO_freq_range(card, &max, &min);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "skiq_read_tx_LO_freq_range failed (card %u), status %d",
                card, status);
            throw std::runtime_error("");
        }
        results.push_back(SoapySDR::Range(min, max));
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return results;
}

/*******************************************************************
 * Sample Rate API
 ******************************************************************/

void SoapySidekiq::writeRxSampleRateAndBandwidth(
        const std::vector<skiq_rx_hdl_t> &handles,
        const uint32_t sample_rate,
        const uint32_t bandwidth)
{
    if (handles.empty())
    {
        SoapySDR_log(SOAPY_SDR_ERROR,
                     "cannot configure RX sample rate without an RX handle");
        throw std::runtime_error("");
    }

    const SoapySDR::Range shared_range =
        intersectSampleRateRanges(this->card, this->param, handles);
    validateRangeValue("RX sample rate", sample_rate, shared_range);

    for (const auto handle : handles)
    {
        validateBuiltInSampleRateIfRequired(
            "RX sample rate",
            this->part,
            sample_rate,
            rxProfileSampleRates(this->part, this->card, this->param, handle));
    }

    validateBandwidthAgainstSampleRate("RX bandwidth", bandwidth, sample_rate);
    if (partRequiresExactBuiltInSampleRate(this->part))
    {
        const std::vector<double> bandwidths = nv100BandwidthsForRate(sample_rate);
        if (!bandwidths.empty() &&
            !rateMatchesProfile(bandwidths, bandwidth))
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "RX bandwidth %u Hz is not one of the documented "
                          "NV100/NVM2 profile percentages for sample rate %u Hz; "
                          "libsidekiq will accept or reject the request",
                          bandwidth,
                          sample_rate);
        }
    }

    int status = 0;
    if (handles.size() == 1)
    {
        status = skiq_write_rx_sample_rate_and_bandwidth(
                this->card, handles.front(), sample_rate, bandwidth);
    }
    else
    {
        std::vector<skiq_rx_hdl_t> mutable_handles(handles.begin(), handles.end());
        std::vector<uint32_t> sample_rates(handles.size(), sample_rate);
        std::vector<uint32_t> bandwidths(handles.size(), bandwidth);

        status = skiq_write_rx_sample_rate_and_bandwidth_multi(
                this->card,
                mutable_handles.data(),
                static_cast<uint8_t>(mutable_handles.size()),
                sample_rates.data(),
                bandwidths.data());
    }

    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "RX sample rate/bandwidth programming failed, "
                      "(card %u, handles %zu, sample_rate %u, bandwidth %u, status %d)",
                      this->card, handles.size(), sample_rate, bandwidth, status);
        throw std::runtime_error("");
    }

    uint32_t configured_rate = sample_rate;
    uint32_t configured_bandwidth = bandwidth;

    for (const auto handle : handles)
    {
        uint32_t actual_rate = 0;
        double fpga_rate = 0;
        uint32_t actual_bw = 0;
        uint32_t fpga_bw = 0;

        status = skiq_read_rx_sample_rate_and_bandwidth(this->card,
                                                        handle,
                                                        &actual_rate,
                                                        &fpga_rate,
                                                        &actual_bw,
                                                        &fpga_bw);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_sample_rate_and_bandwidth failed "
                          "(card %u, handle %u), status %d",
                          this->card, handle, status);
            throw std::runtime_error("");
        }

        if (sample_rate != actual_rate)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "requested RX rate on handle %u: %u, actual rate: %u",
                          handle, sample_rate, actual_rate);
        }

        if (bandwidth != actual_bw)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "requested RX bandwidth on handle %u: %u, actual bandwidth: %u",
                          handle, bandwidth, actual_bw);
        }

        configured_rate = actual_rate;
        configured_bandwidth = actual_bw;
    }

    this->rx_sample_rate = configured_rate;
    this->rx_bandwidth = configured_bandwidth;
}

void SoapySidekiq::setSampleRate(const int direction, const size_t channel,
                                 const double rate)
{
    int status = 0;
    const uint32_t requested_rate = hzToUint32("sample rate", rate);

    SoapySDR_log(SOAPY_SDR_TRACE, "setSampleRate");

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = rxHandleForChannel(channel);
        uint32_t requested_bandwidth = this->rx_bandwidth;
        if (requested_bandwidth > requested_rate)
        {
            requested_bandwidth = requested_rate;
            SoapySDR_logf(SOAPY_SDR_INFO,
                          "clamping RX bandwidth to sample rate while setting rate: %u",
                          requested_bandwidth);
        }

        std::vector<skiq_rx_hdl_t> handles{this->rx_hdl};
        if (!rx_stream_handles.empty() &&
            std::find(rx_stream_handles.begin(), rx_stream_handles.end(),
                      this->rx_hdl) != rx_stream_handles.end())
        {
            handles = rx_stream_handles;
        }

        writeRxSampleRateAndBandwidth(handles, requested_rate, requested_bandwidth);
        SoapySDR_logf(SOAPY_SDR_INFO, "set rx sample rate: %u", this->rx_sample_rate);
    }
    else if (direction == SOAPY_SDR_TX)
    {
        this->tx_hdl = txHandleForChannel(channel);
        validateRangeValue(
            "TX sample rate",
            requested_rate,
            txSampleRateRangeForHandle(this->card, this->param, this->tx_hdl));
        validateBuiltInSampleRateIfRequired(
            "TX sample rate",
            this->part,
            requested_rate,
            txProfileSampleRates(this->part, this->card, this->param, this->tx_hdl));

        this->tx_sample_rate = requested_rate;
        uint32_t requested_bandwidth = this->tx_bandwidth;
        if (requested_bandwidth > this->tx_sample_rate)
        {
            requested_bandwidth = this->tx_sample_rate;
            SoapySDR_logf(SOAPY_SDR_INFO,
                          "clamping TX bandwidth to sample rate while setting rate: %u",
                          requested_bandwidth);
        }

        status = skiq_write_tx_sample_rate_and_bandwidth(this->card,
                                                         this->tx_hdl,
                                                         tx_sample_rate,
                                                         requested_bandwidth);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_tx_sample_rate_and_bandwidth failed, "
                          "(card %u, sample_rate %u, bandwidth %u, status %d)",
                          this->card, tx_sample_rate, requested_bandwidth, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "set tx sample rate: %u", tx_sample_rate);

        // validate that the sample rate was set to what was desired.
        // otherwise log a warning
        uint32_t actual_rate;
        double fpga_rate;
        uint32_t actual_bw;
        uint32_t fpga_bw;

        status = skiq_read_tx_sample_rate_and_bandwidth(this->card,
                                                        this->tx_hdl,
                                                        &actual_rate,
                                                        &fpga_rate,
                                                        &actual_bw,
                                                        &fpga_bw);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "card: %u, skiq_read_tx_sample_rate_and_bandwidth "
                          " failed, status: %d",
                          this->card, status);
            throw std::runtime_error("");
        }

        if (tx_sample_rate != actual_rate)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING, "requested TX rate: %u, is not the same as "
                          "the actual rate: %u",
                          tx_sample_rate, actual_rate);
        }

        this->tx_sample_rate = actual_rate;
        this->tx_bandwidth = actual_bw;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }
}

double SoapySidekiq::getSampleRate(const int    direction,
                                   const size_t channel) const
{
    uint32_t rate;
    double   actual_rate;
    int      status = 0;

    SoapySDR_log(SOAPY_SDR_TRACE, "getSampleRate");

    if (direction == SOAPY_SDR_RX)
    {
        status = skiq_read_rx_sample_rate(card,
                                          rxHandleForChannel(channel),
                                          &rate,
                                          &actual_rate);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                           "skiq_read_rx_sample_rate failed (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        return actual_rate;
    }
    else if (direction == SOAPY_SDR_TX)
    {
        status = skiq_read_tx_sample_rate(card,
                                          txHandleForChannel(channel),
                                          &rate,
                                          &actual_rate);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_sample_rate failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        return actual_rate;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return SoapySDR::Device::getSampleRate(direction, channel);
}

SoapySDR::RangeList SoapySidekiq::getSampleRateRange(const int direction,
                                                     const size_t channel) const
{
    SoapySDR::RangeList ranges;

    SoapySDR_log(SOAPY_SDR_TRACE, "getSampleRateRange");

    if (direction == SOAPY_SDR_RX)
    {
        ranges.push_back(
            rxSampleRateRangeForHandle(card, param, rxHandleForChannel(channel)));
    }
    else if (direction == SOAPY_SDR_TX)
    {
        ranges.push_back(
            txSampleRateRangeForHandle(card, param, txHandleForChannel(channel)));
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return ranges;
}

void SoapySidekiq::setBandwidth(const int direction, const size_t channel,
                                const double bw)
{
    int status = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setBandwidth");

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = rxHandleForChannel(channel);
        const uint32_t requested_bandwidth = hzToUint32("bandwidth", bw);
        std::vector<skiq_rx_hdl_t> handles{this->rx_hdl};
        if (!rx_stream_handles.empty() &&
            std::find(rx_stream_handles.begin(), rx_stream_handles.end(),
                      this->rx_hdl) != rx_stream_handles.end())
        {
            handles = rx_stream_handles;
        }

        validateBandwidthAgainstSampleRate("RX bandwidth",
                                           requested_bandwidth,
                                           this->rx_sample_rate);

        writeRxSampleRateAndBandwidth(
                handles, this->rx_sample_rate, requested_bandwidth);
        SoapySDR_logf(SOAPY_SDR_INFO, "set rx bandwidth to %u", this->rx_bandwidth);
    }
    else if (direction == SOAPY_SDR_TX)
    {
        this->tx_hdl = txHandleForChannel(channel);
        const uint32_t requested_bandwidth = hzToUint32("bandwidth", bw);
        validateBandwidthAgainstSampleRate("TX bandwidth",
                                           requested_bandwidth,
                                           this->tx_sample_rate);

        this->tx_bandwidth = requested_bandwidth;

        status = skiq_write_tx_sample_rate_and_bandwidth(this->card,
                                                         this->tx_hdl,
                                                         this->tx_sample_rate,
                                                         tx_bandwidth);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_tx_sample_rate_and_bandwidth failed, "
                          "(card %u, sample_rate %u, bandwidth %u, status %u)",
                          this->card, this->tx_sample_rate, tx_bandwidth, status);
            throw std::runtime_error("");
        }

        SoapySDR_logf(SOAPY_SDR_INFO, "set tx bandwidth to %u", tx_bandwidth);

        // validate that the bandwidth was set to what was desired.
        // otherwise log a warning
        uint32_t actual_rate;
        double fpga_rate;
        uint32_t actual_bw;
        uint32_t fpga_bw;

        status = skiq_read_tx_sample_rate_and_bandwidth(this->card,
                                                        this->tx_hdl,
                                                        &actual_rate,
                                                        &fpga_rate,
                                                        &actual_bw,
                                                        &fpga_bw);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_tx_sample_rate_and_bandwidth failed "
                          "(card %u, sample_rate %u, bandwidth %u, status %d)",
                          this->card, this->tx_sample_rate, tx_bandwidth, status);
            throw std::runtime_error("");
        }

        if (tx_bandwidth != actual_bw)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING, "requested bandwidth: %u, "
                          " is not the same as actual bandwidth: %u",
                          tx_bandwidth, actual_bw);
        }

        this->tx_sample_rate = actual_rate;
        this->tx_bandwidth = actual_bw;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }
}

/***********************************************************
 * Some open source applications call listSampleRates() when they need concrete
 * choices.  getSampleRateRange() still reports the SDK min/max range for the
 * active handle, but profile-backed radios may only support documented values
 * within that range.  AD9361/AD9364-based cards keep the stepped range fallback.
 */
std::vector<double> SoapySidekiq::listSampleRates(const int direction, const size_t channel) const
{
    if (direction == SOAPY_SDR_RX)
    {
        const skiq_rx_hdl_t handle = rxHandleForChannel(channel);
        const std::vector<double> profile_rates =
            rxProfileSampleRates(part, card, param, handle);
        if (!profile_rates.empty())
        {
            return profile_rates;
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t handle = txHandleForChannel(channel);
        const std::vector<double> profile_rates =
            txProfileSampleRates(part, card, param, handle);
        if (!profile_rates.empty())
        {
            return profile_rates;
        }
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    constexpr double step = 250000.0;
    return steppedValuesForRanges(getSampleRateRange(direction, channel), step);
}

SoapySDR::RangeList SoapySidekiq::getBandwidthRange(const int direction,
                                                     const size_t channel) const
{
    SoapySDR::RangeList ranges;

    SoapySDR_log(SOAPY_SDR_TRACE, "getBandwidthRange");

    if (direction == SOAPY_SDR_RX)
    {
        const skiq_rx_hdl_t handle = rxHandleForChannel(channel);
        if (partRequiresExactBuiltInSampleRate(part))
        {
            const uint32_t rate =
                this->rx_sample_rate == 0 ? DEFAULT_SAMPLE_RATE : this->rx_sample_rate;
            const std::vector<double> bandwidths = nv100BandwidthsForRate(rate);
            if (!bandwidths.empty())
            {
                ranges.push_back(SoapySDR::Range(bandwidths.front(),
                                                 bandwidths.back()));
                return ranges;
            }
        }

        const std::vector<double> profile_bandwidths =
            bandwidthsFromProfilesForRate(rxProfilesForPart(part),
                                          rxHandleMask(handle),
                                          this->rx_sample_rate);
        if (!profile_bandwidths.empty())
        {
            ranges.push_back(SoapySDR::Range(profile_bandwidths.front(),
                                             profile_bandwidths.back()));
            return ranges;
        }

        const SoapySDR::Range sample_range =
            rxSampleRateRangeForHandle(card, param, handle);
        ranges.push_back(SoapySDR::Range(sample_range.minimum(),
                                         this->rx_sample_rate == 0
                                             ? sample_range.maximum()
                                             : std::min<double>(sample_range.maximum(),
                                                                this->rx_sample_rate)));
    }
    else if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t handle = txHandleForChannel(channel);
        if (partRequiresExactBuiltInSampleRate(part))
        {
            const uint32_t rate =
                this->tx_sample_rate == 0 ? DEFAULT_SAMPLE_RATE : this->tx_sample_rate;
            const std::vector<double> bandwidths = nv100BandwidthsForRate(rate);
            if (!bandwidths.empty())
            {
                ranges.push_back(SoapySDR::Range(bandwidths.front(),
                                                 bandwidths.back()));
                return ranges;
            }
        }

        const std::vector<double> profile_bandwidths =
            bandwidthsFromProfilesForRate(txProfilesForPart(part),
                                          txHandleMask(handle),
                                          this->tx_sample_rate);
        if (!profile_bandwidths.empty())
        {
            ranges.push_back(SoapySDR::Range(profile_bandwidths.front(),
                                             profile_bandwidths.back()));
            return ranges;
        }

        const SoapySDR::Range sample_range =
            txSampleRateRangeForHandle(card, param, handle);
        ranges.push_back(SoapySDR::Range(sample_range.minimum(),
                                         this->tx_sample_rate == 0
                                             ? sample_range.maximum()
                                             : std::min<double>(sample_range.maximum(),
                                                                this->tx_sample_rate)));
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return ranges;
}

std::vector<double> SoapySidekiq::listBandwidths(const int direction,
                                                 const size_t channel) const
{
    if (direction == SOAPY_SDR_RX)
    {
        const skiq_rx_hdl_t handle = rxHandleForChannel(channel);
        if (partRequiresExactBuiltInSampleRate(part))
        {
            return nv100BandwidthsForRate(
                this->rx_sample_rate == 0 ? DEFAULT_SAMPLE_RATE : this->rx_sample_rate);
        }

        std::vector<double> profile_bandwidths =
            bandwidthsFromProfilesForRate(rxProfilesForPart(part),
                                          rxHandleMask(handle),
                                          this->rx_sample_rate);
        if (!profile_bandwidths.empty())
        {
            return profile_bandwidths;
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        const skiq_tx_hdl_t handle = txHandleForChannel(channel);
        if (partRequiresExactBuiltInSampleRate(part))
        {
            return nv100BandwidthsForRate(
                this->tx_sample_rate == 0 ? DEFAULT_SAMPLE_RATE : this->tx_sample_rate);
        }

        std::vector<double> profile_bandwidths =
            bandwidthsFromProfilesForRate(txProfilesForPart(part),
                                          txHandleMask(handle),
                                          this->tx_sample_rate);
        if (!profile_bandwidths.empty())
        {
            return profile_bandwidths;
        }
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    constexpr double step = 250000.0;
    return steppedValuesForRanges(getBandwidthRange(direction, channel), step);
}

double SoapySidekiq::getBandwidth(const int    direction,
                                  const size_t channel) const
{
    int status = 0;
    uint32_t rate;
    double   actual_rate;
    uint32_t bandwidth;
    uint32_t actual_bandwidth;

    SoapySDR_log(SOAPY_SDR_TRACE, "getBandwidth");
    if (direction == SOAPY_SDR_RX)
    {
        status = skiq_read_rx_sample_rate_and_bandwidth(card,
                                                   rxHandleForChannel(channel),
                                                   &rate,
                                                   &actual_rate, &bandwidth,
                                                   &actual_bandwidth);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_sample_rate_and_bandwidth failed " 
                          "(card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        status = skiq_read_tx_sample_rate_and_bandwidth(card,
                                                   txHandleForChannel(channel),
                                                   &rate,
                                                   &actual_rate, &bandwidth,
                                                   &actual_bandwidth);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_sample_rate_and_bandwidth failed " 
                          "(card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }

    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return actual_bandwidth;
}

/*******************************************************************
 * Settings API
 ******************************************************************/

SoapySDR::ArgInfoList SoapySidekiq::getSettingInfo(void) const
{
    SoapySDR::ArgInfoList setArgs;
    SoapySDR_logf(SOAPY_SDR_TRACE, "getSettingInfo");

    SoapySDR::ArgInfo settingArg;

    settingArg.key         = "iq_swap";
    settingArg.value       = "true";
    settingArg.name        = "I/Q Swap";
    settingArg.description = "Set I then Q format";
    settingArg.type        = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(settingArg);

    settingArg.key         = "counter";
    settingArg.value       = "false";
    settingArg.name        = "counter";
    settingArg.description = "FPGA creates RAMP samples";
    settingArg.type        = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(settingArg);

    settingArg.key         = "sys_clock_freq";
    settingArg.value       = "sys_freq";
    settingArg.name        = "sys_clock_freq";
    settingArg.description = "The frequency of the system timestamp clock";
    settingArg.type        = SoapySDR::ArgInfo::STRING;
    setArgs.push_back(settingArg);

    settingArg.key         = "timetype";
    settingArg.value       = "rf_timestamp";
    settingArg.name        = "timetype";
    settingArg.description = "The type of timestamp to return in readStream";
    settingArg.type        = SoapySDR::ArgInfo::STRING;
    settingArg.options.push_back("rf_timestamp");
    settingArg.options.push_back("sys_timestamp");
    setArgs.push_back(settingArg);

    return setArgs;
}

void SoapySidekiq::writeSetting(const std::string &key,
                                const std::string &value)
{
    int status = 0;
    skiq_iq_order_t iq_order;

    SoapySDR_logf(SOAPY_SDR_TRACE, "writeSetting");

    // make sure the case of the key doesn't matter
    if (equalsIgnoreCase(key, "iq_swap"))
    {
        this->iq_swap = ((value == "true") ? true : false);

        /* set iq order to iq instead of qi */
        if (iq_swap == true)
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, setting iq mode to I then Q", this->card);
            iq_order = skiq_iq_order_iq;
        }
        else
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, setting iq mode to Q then I", this->card);
            iq_order = skiq_iq_order_qi;
        }

        status = skiq_write_iq_order_mode(this->card, iq_order);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                    "skiq_write_iq_order_mode failed, (card %u), status %d",
                    this->card, status);
                    throw std::runtime_error("");
        }
    }
    // make sure the case of the key doesn't matter
    else if (equalsIgnoreCase(key, "counter"))
    {
        const bool counter_enabled = (value == "true");
        const skiq_data_src_t data_source =
            counter_enabled ? skiq_data_src_counter : skiq_data_src_iq;
        const std::vector<skiq_rx_hdl_t> handles =
            rx_stream_handles.empty()
                ? std::vector<skiq_rx_hdl_t>{rx_hdl}
                : rx_stream_handles;

        for (const auto handle : handles)
        {
            status = skiq_write_rx_data_src(this->card, handle, data_source);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_write_rx_data_src failed, card: %u handle: %u status: %d",
                              this->card, handle, status);
                throw std::runtime_error("");
            }
        }

        counter = counter_enabled;
        SoapySDR_logf(SOAPY_SDR_INFO,
                      "set rx source to %s mode for %zu handle(s)",
                      counter ? "counter" : "normal",
                      handles.size());
    }
    // make sure the case of the key doesn't matter
    else if (equalsIgnoreCase(key, "timetype"))
    {
        if (value == "rf_timestamp")
        {
            timetype = "rf_timestamp";
            rfTimeSource = true;
            SoapySDR_log(SOAPY_SDR_INFO, "set timetype to 'rf_timestamp'");
        }
        else if (value == "sys_timestamp")
        {
            timetype = "sys_timestamp";
            rfTimeSource = false;
            SoapySDR_log(SOAPY_SDR_INFO, "set timetype to 'sys_timestamp'");
        }
        else
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "invalid timetype %s ", value.c_str());
            throw std::runtime_error("");
        }
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_WARNING, "writeSetting invalid key %s ", key.c_str());
    }
}


std::string SoapySidekiq::readSetting(const std::string &key) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "readSetting");

    if (equalsIgnoreCase(key, "iq_swap"))
    {
        return iq_swap ? "true" : "false";
    }
    else if (equalsIgnoreCase(key, "counter"))
    {
        return counter ? "true" : "false";
    }
    else if (equalsIgnoreCase(key, "timetype"))
    {
        return timetype;
    }
    else if (equalsIgnoreCase(key, "sys_clock_freq"))
    {
        return std::to_string((uint64_t)this->sys_freq);
    }
    else if (equalsIgnoreCase(key, "overload"))
    {
        uint8_t overload_state = 0;
        int32_t status = skiq_read_rx_overload_state(this->card,
                                                     this->rx_hdl,
                                                     &overload_state);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_overload_state failed, card: %u status: %d",
                          this->card, status);
            throw std::runtime_error("");
        }
        return std::to_string(overload_state);
    }
    else if (equalsIgnoreCase(key, "cal_offset"))
    {
        double cal_offset = 0;
        int32_t status = skiq_read_rx_cal_offset(this->card,
                                                     this->rx_hdl,
                                                     &cal_offset);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_cal_offset failed, card: %u status: %d",
                          this->card, status);
            throw std::runtime_error("");
        }
        return std::to_string(cal_offset);
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_WARNING, "readSetting invalide key '%s'", key.c_str());
    }
    return "";
}

/*******************************************************************
 * Time API
 ******************************************************************/
#define SOURCE_1PPS_UNAVAILABLE    "1pps_source_unavailable"
#define SOURCE_1PPS_EXTERNAL       "1pps_source_external"
#define SOURCE_1PPS_INTERNAL       "1pps_source_internal"

std::vector<std::string> SoapySidekiq::listTimeSources(void) const
{
    std::vector<std::string> result;
    SoapySDR_logf(SOAPY_SDR_TRACE, "listTimeSources");

    result.push_back(SOURCE_1PPS_UNAVAILABLE);
    result.push_back(SOURCE_1PPS_EXTERNAL);
    result.push_back(SOURCE_1PPS_INTERNAL);

    return result;
}

std::string SoapySidekiq::getTimeSource(void) const
{
    int status = 0;
    skiq_1pps_source_t pps_source;

    SoapySDR_logf(SOAPY_SDR_TRACE, "getTimeSource");

    status = skiq_read_1pps_source(card, &pps_source);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_1pps_source failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }

    switch (pps_source)
    {
        case skiq_1pps_source_unavailable:
            return SOURCE_1PPS_UNAVAILABLE;
            break;

        case skiq_1pps_source_external:
            return SOURCE_1PPS_EXTERNAL;
            break;

        case skiq_1pps_source_host:
            return SOURCE_1PPS_INTERNAL;
            break;

        default:
            SoapySDR_logf(SOAPY_SDR_WARNING, "invalid pps_source %d", pps_source);
            break;
    }

    return "NONE";
}

void check_1pps(uint8_t card, skiq_1pps_source_t pps_source)
{
    int32_t status = 0;
    uint32_t pulseCount = 0;
    uint32_t tryCount = 0;
    uint64_t lastTimestamp = 0;
    uint64_t rfTs = 0;
    uint64_t sysTs = 0;
    bool has_pps_source = false;
    useconds_t one_second_usecs = 1000000;

    SoapySDR_logf(SOAPY_SDR_TRACE, "check_1pps");

    if ((pps_source == skiq_1pps_source_external) || (pps_source == skiq_1pps_source_host))
    {
        do
        {
            status = skiq_read_last_1pps_timestamp(card, &rfTs, &sysTs);
            if (0 != status)
            {
                SoapySDR_logf(SOAPY_SDR_WARNING, "failed to get timestamp for card %" PRIu8
                        " (status = %" PRIi32 "); will try again\n", card, status);
            }
            else
            {
                if (sysTs != lastTimestamp)
                {
                    SoapySDR_logf(SOAPY_SDR_TRACE, "sysTs %lu, " 
                                           "lastTimesamp %lu, trycount %lu, status %d", 
                                           sysTs, lastTimestamp, tryCount, status);
                    pulseCount++;
                    lastTimestamp = sysTs;
                    SoapySDR_logf(SOAPY_SDR_DEBUG, "found 1pps pulse");
                }
            }
            tryCount++;

            if (tryCount > 1)
            {
                if (pulseCount == tryCount)
                {
                    has_pps_source = true; 
                }
            }

            SoapySDR_logf(SOAPY_SDR_DEBUG, "");

            /* sleep for a second */
            usleep(one_second_usecs);

        } while ((has_pps_source == false) && (tryCount <= 1));

        if (has_pps_source == true)
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "Expected 1pps, found 1pps pulse");
        }
        else
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "Expected 1pps, but no 1pps pulse found");
            throw std::runtime_error("");
        } 
    }
}

void SoapySidekiq::setTimeSource(const std::string &source)
{
    int status = 0;
    skiq_1pps_source_t pps_source = skiq_1pps_source_unavailable;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setTimeSource");

    if (equalsIgnoreCase(source, SOURCE_1PPS_UNAVAILABLE))
    {
        pps_source = skiq_1pps_source_unavailable;
    }
    else if (equalsIgnoreCase(source, SOURCE_1PPS_EXTERNAL))
    {
        pps_source = skiq_1pps_source_external;
    }
    else if (equalsIgnoreCase(source, SOURCE_1PPS_INTERNAL))
    {
        pps_source = skiq_1pps_source_host;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_WARNING, "invalid pps_source %s", source.c_str());
    }

    status = skiq_write_1pps_source(card, pps_source);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_write_1pps_source %d failed, (card %u), status %d",
                      pps_source, card, status);
        throw std::runtime_error("");
    }

    SoapySDR_logf(SOAPY_SDR_INFO, "1pps source set to %s", source.c_str());

    check_1pps(card, pps_source);

    return;
}


bool SoapySidekiq::hasHardwareTime(const std::string &what="") const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "hasHardwareTime");

    if (equalsIgnoreCase(what, "rx_rf_timestamp"))
    {
        return true;
    }
    else if (equalsIgnoreCase(what, "tx_rf_timestamp"))
    {
        return true;
    }
    else if (equalsIgnoreCase(what, "sys_timestamp"))
    {
        return true;
    }
    else if (what == "")
    {
        return true;
    }


    return false;
}

/* Return the specific timestamp in nanoseconds */
long long SoapySidekiq::getHardwareTime(const std::string &what="") const
{
    int status = 0;
    uint64_t timestamp = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "getHardwareTime");

    if (equalsIgnoreCase(what, "rx_rf_timestamp"))
    {
        status = skiq_read_curr_rx_timestamp(card, rx_hdl, &timestamp);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_curr_rx_timestamp failed, (card %u), status %d",
                           card, status);
            throw std::runtime_error("");
        }

        // if the user has not set the sample rate then we
        // must warn them and return
        if (rx_sample_rate == 0)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING, "Cannot convert an rf timestamp to time without"
                          "configuring the sample rate first.  Passing unconverted timestamp");

            return timestamp;
        }

        return convert_timestamp_to_nanos(timestamp, rx_sample_rate);
    }
    else if (equalsIgnoreCase(what, "tx_rf_timestamp"))
    {
        status = skiq_read_curr_tx_timestamp(card, tx_hdl, &timestamp);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_curr_rx_timestamp failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }

        // if the user has not set the sample rate then we
        // must warn them and return
        if (tx_sample_rate == 0)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING, "Cannot convert an rf timestamp to time without"
                          "configuring the sample rate first.  Passing unconverted timestamp");

            return timestamp;
        }
        return convert_timestamp_to_nanos(timestamp, tx_sample_rate);
    }
    else if (equalsIgnoreCase(what, "sys_timestamp"))
    {
        status = skiq_read_curr_sys_timestamp(card, &timestamp);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_curr_sys_timestamp failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        return convert_timestamp_to_nanos(timestamp, this->sys_freq);

    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_WARNING, "invalid 'what' parameter %s", what.c_str());
    }

    return 0;
}

void SoapySidekiq::setHardwareTime(const long long timeNs, const std::string &what="")
{
    int status = 0;
    double double_timestamp = 0;
    uint64_t new_timestamp = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setHardwareTime");

    // convert timeNs to sys timestamp frequency
    // we only care about setting the sys_timestamp but we have to set both
    // given the API call
    double_timestamp = (double)timeNs * (double)this->sys_freq / (double)NANOS_IN_SEC;
    new_timestamp = (uint64_t)double_timestamp;

    if (equalsIgnoreCase(what, "now"))
    {
        status = skiq_update_timestamps(card, new_timestamp);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_update_timestamps failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }

        SoapySDR_logf(SOAPY_SDR_INFO, "card %u, updated both timestamps to %lld",
                      card, new_timestamp);

        return;
    }
    else if (equalsIgnoreCase(what, "1pps"))
    {
        status = skiq_write_timestamp_update_on_1pps(card, 0, new_timestamp);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_update_timestamp_on_1pps failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "card %d, updated timestamps to %lu on the next 1pps",
                      card, new_timestamp);

        return;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_WARNING, "invalid 'what' parameter %s", what.c_str());
    }

    return;
}

/*******************************************************************
 * Clocking API
 ******************************************************************/

std::vector<std::string> SoapySidekiq::listClockSources(void) const
{
    std::vector<std::string> result;
    SoapySDR_logf(SOAPY_SDR_TRACE, "listClockSources");

    result.push_back("external_clock");
    result.push_back("internal_clock");

    return result;
}

std::string SoapySidekiq::getClockSource(void) const
{
    int status = 0;
    skiq_ref_clock_select_t ref_clock = skiq_ref_clock_internal;

    SoapySDR_logf(SOAPY_SDR_TRACE, "getClockSource");

    status = skiq_read_ref_clock_select(card, &ref_clock);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_ref_clock_select failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }

    if ((ref_clock == skiq_ref_clock_internal) || (ref_clock == skiq_ref_clock_host))
    {
        return "internal_clock";
    }
    else if (ref_clock == skiq_ref_clock_external)
    {
        return "external_clock";
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid ref clock %d", ref_clock);
        throw std::runtime_error("");
    }

    return "NONE";
}

void SoapySidekiq::setClockSource(const std::string &source)
{
    int status = 0;
    skiq_ref_clock_select_t ref_clock = skiq_ref_clock_internal;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setClockSource");

    if (equalsIgnoreCase(source, "internal_clock"))
    {
        ref_clock = skiq_ref_clock_internal;
    }
    else if (equalsIgnoreCase(source, "external_clock"))
    {
        ref_clock = skiq_ref_clock_external;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_WARNING, "invalid ref_clock %s", source.c_str());
    }

    status = skiq_write_ref_clock_select(card, ref_clock);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_write_ref_clock_select failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }

    SoapySDR_logf(SOAPY_SDR_INFO, "ref_clock set to %s", source.c_str());
    return;
}

double SoapySidekiq::getReferenceClockRate(void) const
{
    int status = 0;
    std::string source;
    uint32_t ref_clock = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "getReferenceClockRate");

    // get the current ref clock
    source = getClockSource();

    if (equalsIgnoreCase(source, "external_clock"))
    {
        status = skiq_read_ext_ref_clock_freq(card, &ref_clock);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_ref_clock_select failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        return ref_clock;
    }
    else if (equalsIgnoreCase(source, "internal_clock"))
    {
        return this->param.rf_param.ref_clock_freq;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid ref clock %d", ref_clock);
        throw std::runtime_error("");
    }

    return 0;
}
