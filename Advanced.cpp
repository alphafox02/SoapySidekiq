// Per-channel settings for libsidekiq features without a SoapySDR API of
// their own: frequency hopping, RF filter path selection and FIR access.
// Device-wide user calibration files are handled here as well.

#include "SoapySidekiq.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

bool equalsIgnoreCase(const std::string &a, const std::string &b);

namespace
{
constexpr const char *KEY_TUNE_MODE = "freq_tune_mode";
constexpr const char *KEY_HOP_LIST = "freq_hop_list";
constexpr const char *KEY_HOP_NEXT = "freq_hop_next";
constexpr const char *KEY_HOP_PERFORM = "freq_hop_perform";
constexpr const char *KEY_HOP_CURRENT = "freq_hop_current";
constexpr const char *KEY_RF_FILTER = "rf_filter";
constexpr const char *KEY_FIR_GAIN = "fir_gain";
constexpr const char *KEY_FIR_CONFIG = "fir_config";
constexpr const char *KEY_FIR_COEFFS = "fir_coeffs";

void checkStatus(const int status, const std::string &what)
{
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "%s failed, status %d", what.c_str(), status);
        throw std::runtime_error(what + " failed (status " + std::to_string(status) + ")");
    }
}

const char *tuneModeName(const skiq_freq_tune_mode_t mode)
{
    switch (mode)
    {
        case skiq_freq_tune_mode_hop_immediate: return "hop_immediate";
        case skiq_freq_tune_mode_hop_on_timestamp: return "hop_on_timestamp";
        default: return "standard";
    }
}

skiq_freq_tune_mode_t tuneModeFromName(const std::string &name)
{
    if (equalsIgnoreCase(name, "standard")) return skiq_freq_tune_mode_standard;
    if (equalsIgnoreCase(name, "hop_immediate")) return skiq_freq_tune_mode_hop_immediate;
    if (equalsIgnoreCase(name, "hop_on_timestamp")) return skiq_freq_tune_mode_hop_on_timestamp;
    throw std::runtime_error("unknown freq_tune_mode '" + name +
                             "' (standard, hop_immediate, hop_on_timestamp)");
}

std::vector<std::string> splitList(const std::string &text)
{
    std::vector<std::string> items;
    std::string item;
    std::istringstream stream(text);
    while (std::getline(stream, item, ','))
    {
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty())
        {
            items.push_back(item);
        }
    }
    return items;
}

double parseNumber(const std::string &text, const std::string &what)
{
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value))
    {
        throw std::runtime_error("invalid " + what + " '" + text + "'");
    }
    return value;
}

// Filter names are the frequency span, e.g. "440-580MHz"; "auto" selects the
// filter covering the current LO frequency (what libsidekiq does on retune).
std::string filterName(const skiq_filt_t filter)
{
    uint64_t start = 0;
    uint64_t end = 0;
    if (skiq_read_filter_range(filter, &start, &end) != 0)
    {
        return "filter_" + std::to_string(static_cast<int>(filter));
    }
    return std::to_string(start / 1000000) + "-" + std::to_string(end / 1000000) + "MHz";
}

template <typename Joinable>
std::string joinList(const Joinable &values)
{
    std::ostringstream stream;
    bool first = true;
    for (const auto &value : values)
    {
        stream << (first ? "" : ",") << value;
        first = false;
    }
    return stream.str();
}
}

/*******************************************************************
 * Channel settings API
 ******************************************************************/

SoapySDR::ArgInfoList SoapySidekiq::getSettingInfo(const int direction,
                                                   const size_t channel) const
{
    SoapySDR::ArgInfoList args;
    const bool rx = direction == SOAPY_SDR_RX;

    SoapySDR::ArgInfo arg;
    arg.key = KEY_TUNE_MODE;
    arg.name = "Frequency Tune Mode";
    arg.description = "standard: setFrequency() tunes the LO. hop_immediate / "
                      "hop_on_timestamp: hardware frequency hopping through the "
                      "list in freq_hop_list.  Set before activateStream()";
    arg.type = SoapySDR::ArgInfo::STRING;
    arg.value = "standard";
    arg.options = {"standard", "hop_immediate", "hop_on_timestamp"};
    args.push_back(arg);

    arg = SoapySDR::ArgInfo();
    arg.key = KEY_HOP_LIST;
    arg.name = "Frequency Hop List";
    arg.description = "Comma-separated frequencies in Hz (up to " +
                      std::to_string(SKIQ_MAX_NUM_FREQ_HOPS - 1) +
                      "); set freq_tune_mode to a hopping mode first.  Writing "
                      "the list makes its first entry the pending hop";
    arg.type = SoapySDR::ArgInfo::STRING;
    arg.units = "Hz";
    args.push_back(arg);

    arg = SoapySDR::ArgInfo();
    arg.key = KEY_HOP_NEXT;
    arg.name = "Next Hop Index";
    arg.description = "Index in freq_hop_list to queue for the hop after the "
                      "pending one.  libsidekiq keeps one hop pending: write this "
                      "before each freq_hop_perform (the pending hop executes, and "
                      "this index becomes pending).  NV100/NVM2 advance through the "
                      "list on their own and do not support it";
    arg.type = SoapySDR::ArgInfo::INT;
    args.push_back(arg);

    arg = SoapySDR::ArgInfo();
    arg.key = KEY_HOP_PERFORM;
    arg.name = "Perform Hop";
    arg.description = "Write to execute the pending hop.  In hop_on_timestamp "
                      "mode the value is the time in nanoseconds on the RF "
                      "timestamp clock (the timebase of readStream() timeNs); "
                      "0 or a past time hops immediately";
    arg.type = SoapySDR::ArgInfo::INT;
    arg.units = "ns";
    args.push_back(arg);

    arg = SoapySDR::ArgInfo();
    arg.key = KEY_HOP_CURRENT;
    arg.name = "Current Hop Index";
    arg.description = "Read only: index of the frequency the hardware is on";
    arg.type = SoapySDR::ArgInfo::INT;
    args.push_back(arg);

    arg = SoapySDR::ArgInfo();
    arg.key = KEY_RF_FILTER;
    arg.name = rx ? "RX Preselect Filter" : "TX Filter";
    arg.description = "RF filter path; \"auto\" selects the filter covering the "
                      "current frequency, which is also what happens on every retune";
    arg.type = SoapySDR::ArgInfo::STRING;
    arg.value = "auto";
    arg.options.push_back("auto");
    try
    {
        std::vector<skiq_filt_t> filters(skiq_filt_max);
        uint8_t count = 0;
        const int status = rx
            ? skiq_read_rx_filters_avail(card, rxHandleForChannel(channel),
                                         filters.data(), &count)
            : skiq_read_tx_filters_avail(card, txHandleForChannel(channel),
                                         filters.data(), &count);
        if (status == 0)
        {
            for (uint8_t i = 0; i < count; i++)
            {
                arg.options.push_back(filterName(filters[i]));
            }
        }
    }
    catch (const std::exception &)
    {
    }
    args.push_back(arg);

    arg = SoapySDR::ArgInfo();
    arg.key = KEY_FIR_GAIN;
    arg.name = "FIR Gain";
    arg.description = "Gain of the RFIC's digital FIR filter";
    arg.type = SoapySDR::ArgInfo::STRING;
    arg.units = "dB";
    arg.options = rx ? std::vector<std::string>{"-12", "-6", "0", "6"}
                     : std::vector<std::string>{"-6", "0"};
    args.push_back(arg);

    arg = SoapySDR::ArgInfo();
    arg.key = KEY_FIR_CONFIG;
    arg.name = "FIR Configuration";
    arg.description = "Read only: number of FIR taps and the decimation (RX) or "
                      "interpolation (TX) factor";
    arg.type = SoapySDR::ArgInfo::STRING;
    args.push_back(arg);

    arg = SoapySDR::ArgInfo();
    arg.key = KEY_FIR_COEFFS;
    arg.name = "FIR Coefficients";
    arg.description = "Comma-separated int16 FIR coefficients (one per tap, see "
                      "fir_config).  Shared by all channels of the RFIC and "
                      "replaced by any sample-rate or bandwidth change.  Epiq "
                      "advises against writing these; for experts only";
    arg.type = SoapySDR::ArgInfo::STRING;
    args.push_back(arg);

    return args;
}

void SoapySidekiq::writeSetting(const int direction, const size_t channel,
                                const std::string &key, const std::string &value)
{
    const bool rx = direction == SOAPY_SDR_RX;
    if (!rx && direction != SOAPY_SDR_TX)
    {
        throw std::runtime_error("invalid direction");
    }
    const skiq_rx_hdl_t rx_handle = rx ? rxHandleForChannel(channel) : skiq_rx_hdl_end;
    const skiq_tx_hdl_t tx_handle = rx ? skiq_tx_hdl_end : txHandleForChannel(channel);
    const char *dir = rx ? "RX" : "TX";

    // Measured on a Sidekiq Stretch: changing the tune mode or hop list while
    // the channel streams leaves the RFIC mistuned (libsidekiq reports the
    // new frequency, the RF does not follow).  Configured before the stream
    // starts, hops performed while streaming work correctly.
    if (equalsIgnoreCase(key, KEY_TUNE_MODE) || equalsIgnoreCase(key, KEY_HOP_LIST))
    {
        const bool streaming = rx
            ? (rx_running && std::find(rx_stream_handles.begin(), rx_stream_handles.end(),
                                       rx_handle) != rx_stream_handles.end())
            : (tx_stream_active && tx_hdl == tx_handle);
        if (streaming)
        {
            throw std::runtime_error(key + " cannot be changed while the " + dir +
                                     " stream is active; set freq_tune_mode and "
                                     "freq_hop_list before activateStream() (hops "
                                     "themselves can be performed while streaming)");
        }
    }

    if (equalsIgnoreCase(key, KEY_TUNE_MODE))
    {
        const skiq_freq_tune_mode_t mode = tuneModeFromName(value);
        checkStatus(rx ? skiq_write_rx_freq_tune_mode(card, rx_handle, mode)
                       : skiq_write_tx_freq_tune_mode(card, tx_handle, mode),
                    std::string("setting the ") + dir + " frequency tune mode");
        SoapySDR_logf(SOAPY_SDR_INFO, "%s channel %zu tune mode: %s",
                      dir, channel, tuneModeName(mode));
        if (mode == skiq_freq_tune_mode_hop_on_timestamp && part == skiq_m2_2280)
        {
            // measured with libsidekiq v4.26.0 / FPGA v3.21.0: the hop is
            // reported but the RF does not retune; hop_immediate works
            SoapySDR_log(SOAPY_SDR_WARNING,
                         "hop_on_timestamp did not retune the RF on a Sidekiq Stretch in "
                         "testing (libsidekiq v4.26.0); use hop_immediate if hops have "
                         "no effect");
        }
    }
    else if (equalsIgnoreCase(key, KEY_HOP_LIST))
    {
        std::vector<uint64_t> freqs;
        for (const auto &item : splitList(value))
        {
            const double hz = parseNumber(item, "hop frequency");
            if (hz <= 0)
            {
                throw std::runtime_error("hop frequencies must be positive: " + item);
            }
            freqs.push_back(static_cast<uint64_t>(std::llround(hz)));
        }
        if (freqs.empty() || freqs.size() >= SKIQ_MAX_NUM_FREQ_HOPS)
        {
            throw std::runtime_error("freq_hop_list needs 1 to " +
                                     std::to_string(SKIQ_MAX_NUM_FREQ_HOPS - 1) +
                                     " frequencies");
        }
        const uint16_t count = static_cast<uint16_t>(freqs.size());
        const int status = rx
            ? skiq_write_rx_freq_hop_list(card, rx_handle, count, freqs.data(), 0)
            : skiq_write_tx_freq_hop_list(card, tx_handle, count, freqs.data(), 0);
        if (status == -EPROTO)
        {
            throw std::runtime_error("set freq_tune_mode to hop_immediate or "
                                     "hop_on_timestamp before writing freq_hop_list");
        }
        checkStatus(status, std::string("writing the ") + dir + " hop list");
        SoapySDR_logf(SOAPY_SDR_INFO, "%s channel %zu hop list: %u frequencies",
                      dir, channel, count);
    }
    else if (equalsIgnoreCase(key, KEY_HOP_NEXT))
    {
        const double index = parseNumber(value, "hop index");
        if (index < 0 || index >= SKIQ_MAX_NUM_FREQ_HOPS || index != std::floor(index))
        {
            throw std::runtime_error("invalid hop index '" + value + "'");
        }
        checkStatus(rx ? skiq_write_next_rx_freq_hop(card, rx_handle, static_cast<uint16_t>(index))
                       : skiq_write_next_tx_freq_hop(card, tx_handle, static_cast<uint16_t>(index)),
                    std::string("preparing the next ") + dir + " hop");
    }
    else if (equalsIgnoreCase(key, KEY_HOP_PERFORM))
    {
        // nanoseconds on the RF timestamp clock -> RF timestamp ticks
        const double ns = value.empty() ? 0.0 : parseNumber(value, "hop time");
        const uint32_t rate = rx ? rx_sample_rate_by_handle[rx_handle]
                                 : tx_sample_rate_by_handle[tx_handle];
        const uint64_t ticks = (ns <= 0 || rate == 0)
            ? 0
            : static_cast<uint64_t>(std::llround(ns * rate / 1e9));
        const int status = rx ? skiq_perform_rx_freq_hop(card, rx_handle, ticks)
                              : skiq_perform_tx_freq_hop(card, tx_handle, ticks);
        if (status == -ENODEV)
        {
            throw std::runtime_error("no hop is pending: write freq_hop_next before "
                                     "each freq_hop_perform (or rewrite freq_hop_list)");
        }
        checkStatus(status, std::string("performing the ") + dir + " hop");
    }
    else if (equalsIgnoreCase(key, KEY_RF_FILTER))
    {
        if (equalsIgnoreCase(value, "auto"))
        {
            // libsidekiq picks the filter for the LO on every tune
            setFrequency(direction, channel, getFrequency(direction, channel));
            return;
        }

        std::vector<skiq_filt_t> filters(skiq_filt_max);
        uint8_t count = 0;
        checkStatus(rx ? skiq_read_rx_filters_avail(card, rx_handle, filters.data(), &count)
                       : skiq_read_tx_filters_avail(card, tx_handle, filters.data(), &count),
                    std::string("reading the ") + dir + " filters");
        for (uint8_t i = 0; i < count; i++)
        {
            if (equalsIgnoreCase(value, filterName(filters[i])))
            {
                checkStatus(rx ? skiq_write_rx_preselect_filter_path(card, rx_handle, filters[i])
                               : skiq_write_tx_filter_path(card, tx_handle, filters[i]),
                            std::string("selecting the ") + dir + " filter");
                SoapySDR_logf(SOAPY_SDR_INFO, "%s channel %zu filter: %s",
                              dir, channel, value.c_str());
                return;
            }
        }
        throw std::runtime_error("filter '" + value + "' is not available on this channel");
    }
    else if (equalsIgnoreCase(key, KEY_FIR_GAIN))
    {
        const int db = static_cast<int>(parseNumber(value, "FIR gain"));
        int status = -EINVAL;
        if (rx)
        {
            const skiq_rx_fir_gain_t gain =
                db == 6 ? skiq_rx_fir_gain_6 : db == 0 ? skiq_rx_fir_gain_0 :
                db == -6 ? skiq_rx_fir_gain_neg_6 : db == -12 ? skiq_rx_fir_gain_neg_12 :
                static_cast<skiq_rx_fir_gain_t>(-1);
            if (static_cast<int>(gain) < 0)
            {
                throw std::runtime_error("RX FIR gain must be -12, -6, 0 or 6 dB");
            }
            status = skiq_write_rx_fir_gain(card, rx_handle, gain);
        }
        else
        {
            if (db != 0 && db != -6)
            {
                throw std::runtime_error("TX FIR gain must be -6 or 0 dB");
            }
            status = skiq_write_tx_fir_gain(card, tx_handle,
                                            db == 0 ? skiq_tx_fir_gain_0 : skiq_tx_fir_gain_neg_6);
        }
        checkStatus(status, std::string("setting the ") + dir + " FIR gain");
    }
    else if (equalsIgnoreCase(key, KEY_FIR_COEFFS))
    {
        uint8_t taps = 0;
        uint8_t factor = 0;
        checkStatus(rx ? skiq_read_rfic_rx_fir_config(card, &taps, &factor)
                       : skiq_read_rfic_tx_fir_config(card, &taps, &factor),
                    std::string("reading the ") + dir + " FIR configuration");
        const std::vector<std::string> items = splitList(value);
        if (items.size() != taps)
        {
            throw std::runtime_error("the " + std::string(dir) + " FIR has " +
                                     std::to_string(taps) + " taps but " +
                                     std::to_string(items.size()) +
                                     " coefficients were given");
        }
        std::vector<int16_t> coeffs;
        for (const auto &item : items)
        {
            const double c = parseNumber(item, "FIR coefficient");
            if (c < -32768 || c > 32767 || c != std::floor(c))
            {
                throw std::runtime_error("FIR coefficients must be int16 values: " + item);
            }
            coeffs.push_back(static_cast<int16_t>(c));
        }
        checkStatus(rx ? skiq_write_rfic_rx_fir_coeffs(card, coeffs.data())
                       : skiq_write_rfic_tx_fir_coeffs(card, coeffs.data()),
                    std::string("writing the ") + dir + " FIR coefficients");
        SoapySDR_logf(SOAPY_SDR_WARNING,
                      "custom %s FIR coefficients written; a sample-rate or bandwidth "
                      "change replaces them", dir);
    }
    else
    {
        SoapySDR::Device::writeSetting(direction, channel, key, value);
    }
}

std::string SoapySidekiq::readSetting(const int direction, const size_t channel,
                                      const std::string &key) const
{
    const bool rx = direction == SOAPY_SDR_RX;
    if (!rx && direction != SOAPY_SDR_TX)
    {
        throw std::runtime_error("invalid direction");
    }
    const skiq_rx_hdl_t rx_handle = rx ? rxHandleForChannel(channel) : skiq_rx_hdl_end;
    const skiq_tx_hdl_t tx_handle = rx ? skiq_tx_hdl_end : txHandleForChannel(channel);
    const char *dir = rx ? "RX" : "TX";

    if (equalsIgnoreCase(key, KEY_TUNE_MODE))
    {
        skiq_freq_tune_mode_t mode = skiq_freq_tune_mode_standard;
        checkStatus(rx ? skiq_read_rx_freq_tune_mode(card, rx_handle, &mode)
                       : skiq_read_tx_freq_tune_mode(card, tx_handle, &mode),
                    std::string("reading the ") + dir + " tune mode");
        return tuneModeName(mode);
    }
    if (equalsIgnoreCase(key, KEY_HOP_LIST))
    {
        std::vector<uint64_t> freqs(SKIQ_MAX_NUM_FREQ_HOPS);
        uint16_t count = 0;
        const int status = rx
            ? skiq_read_rx_freq_hop_list(card, rx_handle, &count, freqs.data())
            : skiq_read_tx_freq_hop_list(card, tx_handle, &count, freqs.data());
        if (status != 0)
        {
            return "";
        }
        freqs.resize(count);
        return joinList(freqs);
    }
    if (equalsIgnoreCase(key, KEY_HOP_NEXT) || equalsIgnoreCase(key, KEY_HOP_CURRENT))
    {
        const bool next = equalsIgnoreCase(key, KEY_HOP_NEXT);
        uint16_t index = 0;
        uint64_t freq = 0;
        const int status = rx
            ? (next ? skiq_read_next_rx_freq_hop(card, rx_handle, &index, &freq)
                    : skiq_read_curr_rx_freq_hop(card, rx_handle, &index, &freq))
            : (next ? skiq_read_next_tx_freq_hop(card, tx_handle, &index, &freq)
                    : skiq_read_curr_tx_freq_hop(card, tx_handle, &index, &freq));
        return status == 0 ? std::to_string(index) : "";
    }
    if (equalsIgnoreCase(key, KEY_RF_FILTER))
    {
        skiq_filt_t filter = skiq_filt_invalid;
        const int status = rx ? skiq_read_rx_preselect_filter_path(card, rx_handle, &filter)
                              : skiq_read_tx_filter_path(card, tx_handle, &filter);
        return (status == 0 && filter != skiq_filt_invalid) ? filterName(filter) : "";
    }
    if (equalsIgnoreCase(key, KEY_FIR_GAIN))
    {
        if (rx)
        {
            skiq_rx_fir_gain_t gain = skiq_rx_fir_gain_0;
            checkStatus(skiq_read_rx_fir_gain(card, rx_handle, &gain), "reading the RX FIR gain");
            return gain == skiq_rx_fir_gain_6 ? "6" : gain == skiq_rx_fir_gain_neg_6 ? "-6" :
                   gain == skiq_rx_fir_gain_neg_12 ? "-12" : "0";
        }
        skiq_tx_fir_gain_t gain = skiq_tx_fir_gain_0;
        checkStatus(skiq_read_tx_fir_gain(card, tx_handle, &gain), "reading the TX FIR gain");
        return gain == skiq_tx_fir_gain_neg_6 ? "-6" : "0";
    }
    if (equalsIgnoreCase(key, KEY_FIR_CONFIG) || equalsIgnoreCase(key, KEY_FIR_COEFFS))
    {
        uint8_t taps = 0;
        uint8_t factor = 0;
        checkStatus(rx ? skiq_read_rfic_rx_fir_config(card, &taps, &factor)
                       : skiq_read_rfic_tx_fir_config(card, &taps, &factor),
                    std::string("reading the ") + dir + " FIR configuration");
        if (equalsIgnoreCase(key, KEY_FIR_CONFIG))
        {
            return "taps=" + std::to_string(taps) + (rx ? ",decimation=" : ",interpolation=") +
                   std::to_string(factor);
        }
        // sized for the largest tap count the API can report
        std::vector<int16_t> coeffs(256);
        checkStatus(rx ? skiq_read_rfic_rx_fir_coeffs(card, coeffs.data())
                       : skiq_read_rfic_tx_fir_coeffs(card, coeffs.data()),
                    std::string("reading the ") + dir + " FIR coefficients");
        coeffs.resize(taps);
        return joinList(coeffs);
    }

    return SoapySDR::Device::readSetting(direction, channel, key);
}
