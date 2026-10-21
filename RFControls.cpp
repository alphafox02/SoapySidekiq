/**
 * @file RFControls.cpp
 * @brief Implements per-channel RF controls and capability reporting.
 */

#include "SoapySidekiq.hpp"
#include <SoapySidekiq/NumericConversion.hpp>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

/*******************************************************************
 * Antenna API
 ******************************************************************/

std::vector<std::string> SoapySidekiq::listAntennas(const int direction, const size_t channel) const
{
    const auto name = currentAntenna(direction, channel);
    return name.empty() ? std::vector<std::string>{} : std::vector<std::string>{name};
}

std::string SoapySidekiq::currentAntenna(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    skiq_rf_port_t port = skiq_rf_port_unknown;
    const int status = direction == SOAPY_SDR_RX
                           ? skiq_read_rx_rf_port_for_hdl(card, getRxHandle(channel), &port)
                           : skiq_read_tx_rf_port_for_hdl(card, getTxHandle(channel), &port);
    // RF-port reporting is optional; do not advertise a fabricated choice.
    if (status == -ENOTSUP || status == -ENOSYS)
        return {};
    if (status != 0)
        throw std::runtime_error(std::string(direction == SOAPY_SDR_RX ? "RX" : "TX") +
                                 " RF-port read failed (card " + std::to_string(card) +
                                 ", channel " + std::to_string(channel) + ", status " +
                                 std::to_string(status) + ")");
    if (port == skiq_rf_port_unknown)
        return {};
    const char *name = skiq_rf_port_string(port);
    if (name == nullptr || name[0] == '\0')
        throw std::runtime_error("SDK returned an invalid RF-port name (card " +
                                 std::to_string(card) + ", channel " + std::to_string(channel) +
                                 ")");
    return name;
}

std::string SoapySidekiq::getAntenna(const int direction, const size_t channel) const
{
    return currentAntenna(direction, channel);
}

void SoapySidekiq::setAntenna(const int direction, const size_t channel, const std::string &name)
{
    const auto current = currentAntenna(direction, channel);
    // The driver currently exposes only the configured route, so selecting it
    // is idempotent. Other selections must not silently succeed via the base API.
    if (current.empty() || name != current)
        throw std::invalid_argument("antenna selection '" + name + "' is unavailable (card " +
                                    std::to_string(card) + ", channel " + std::to_string(channel) +
                                    "); only the configured RF port can be selected");
}

/*******************************************************************
 * Frontend corrections API
 ******************************************************************/

bool SoapySidekiq::hasDCOffsetMode(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    if (direction == SOAPY_SDR_TX)
        return false;
    const auto handle = getRxHandle(channel);
    uint32_t available{};
    const int status = skiq_read_rx_cal_types_avail(card, handle, &available);
    if (status != 0)
        throw std::runtime_error("skiq_read_rx_cal_types_avail failed (card " +
                                 std::to_string(card) + ", channel " + std::to_string(channel) +
                                 ", handle " + std::to_string(handle) + ", status " +
                                 std::to_string(status) + ")");
    return (available & skiq_rx_cal_type_dc_offset) != 0;
}

void SoapySidekiq::setDCOffsetMode(const int direction, const size_t channel, const bool automatic)
{
    validateChannel(direction, channel);
    if (!hasDCOffsetMode(direction, channel))
    {
        if (automatic)
            throw std::invalid_argument("automatic DC-offset correction is unavailable (card " +
                                        std::to_string(card) + ", direction " +
                                        std::to_string(direction) + ", channel " +
                                        std::to_string(channel) + ")");
        return;
    }
    const auto handle = getRxHandle(channel);
    const auto context = " (card " + std::to_string(card) + ", channel " + std::to_string(channel) +
                         ", handle " + std::to_string(handle) + ")";
    const auto check = [&](const int status, const char *operation)
    {
        if (status != 0)
            throw std::runtime_error(std::string(operation) + context + " failed, status " +
                                     std::to_string(status));
    };
    uint32_t original_mask{};
    check(skiq_read_rx_cal_type_mask(card, handle, &original_mask), "skiq_read_rx_cal_type_mask");
    const uint32_t desired_mask =
        automatic ? original_mask | skiq_rx_cal_type_dc_offset
                  : original_mask & ~static_cast<uint32_t>(skiq_rx_cal_type_dc_offset);
    skiq_rx_cal_mode_t original_mode = skiq_rx_cal_mode_manual;
    if (automatic)
    {
        check(skiq_read_rx_cal_mode(card, handle, &original_mode), "skiq_read_rx_cal_mode");
        if (original_mode != skiq_rx_cal_mode_manual && original_mode != skiq_rx_cal_mode_auto)
            throw std::runtime_error("unsupported RX calibration mode" + context);
    }
    if (desired_mask != original_mask)
    {
        const int status = skiq_write_rx_cal_type_mask(card, handle, desired_mask);
        if (status != 0)
        {
            const int restore = skiq_write_rx_cal_type_mask(card, handle, original_mask);
            throw std::runtime_error("skiq_write_rx_cal_type_mask" + context + " failed, status " +
                                     std::to_string(status) + "; rollback mask status " +
                                     std::to_string(restore));
        }
    }

    // Disable only the DC bit: the shared calibration mode and other selected
    // algorithms belong to the rest of the RF configuration too.
    if (!automatic || original_mode == skiq_rx_cal_mode_auto)
        return;
    const int status = skiq_write_rx_cal_mode(card, handle, skiq_rx_cal_mode_auto);
    if (status != 0)
    {
        // Restore the snapshot after a failed second configuration step. SDK
        // errors cannot guarantee atomic hardware updates, so report failed
        // compensation as well as the original failure.
        const int mode_restore = skiq_write_rx_cal_mode(card, handle, original_mode);
        const int mask_restore = desired_mask == original_mask
                                     ? 0
                                     : skiq_write_rx_cal_type_mask(card, handle, original_mask);
        throw std::runtime_error("skiq_write_rx_cal_mode" + context + " failed, status " +
                                 std::to_string(status) + "; rollback mode status " +
                                 std::to_string(mode_restore) + ", mask status " +
                                 std::to_string(mask_restore));
    }
}

bool SoapySidekiq::getDCOffsetMode(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    if (!hasDCOffsetMode(direction, channel))
        return false;
    const auto handle = getRxHandle(channel);
    const auto context = " (card " + std::to_string(card) + ", channel " + std::to_string(channel) +
                         ", handle " + std::to_string(handle) + ")";
    uint32_t mask{};
    int status = skiq_read_rx_cal_type_mask(card, handle, &mask);
    if (status != 0)
        throw std::runtime_error("skiq_read_rx_cal_type_mask" + context + " failed, status " +
                                 std::to_string(status));
    if ((mask & skiq_rx_cal_type_dc_offset) == 0)
        return false;
    skiq_rx_cal_mode_t mode = skiq_rx_cal_mode_manual;
    status = skiq_read_rx_cal_mode(card, handle, &mode);
    if (status != 0)
        throw std::runtime_error("skiq_read_rx_cal_mode" + context + " failed, status " +
                                 std::to_string(status));
    if (mode != skiq_rx_cal_mode_auto && mode != skiq_rx_cal_mode_manual)
        throw std::runtime_error("unsupported RX calibration mode" + context);
    return mode == skiq_rx_cal_mode_auto;
}

/*******************************************************************
 * Gain API
 ******************************************************************/

SoapySidekiq::GainProfile SoapySidekiq::gainProfile(const size_t channel) const
{
    const auto handle = getRxHandle(channel);
    double steps_per_db;
    switch (part)
    {
    case skiq_mpcie:
    case skiq_m2:
    case skiq_m2_2280:
    case skiq_z2:
    case skiq_z3u:
        steps_per_db = 1;
        break;
    case skiq_x2:
        // X2 B1 is the observation receiver: its gain table uses 1 dB steps.
        steps_per_db = handle == skiq_rx_hdl_B1 ? 1 : 2;
        break;
    case skiq_x4:
    case skiq_x40:
    case skiq_nv100:
    case skiq_nvm2:
    case skiq_z4:
    case skiq_z4_mp:
        steps_per_db = 2;
        break;
    default:
        throw std::runtime_error("unsupported Sidekiq gain profile (card " + std::to_string(card) +
                                 ")");
    }

    uint8_t minimum{}, maximum{};
    const auto context = " (card " + std::to_string(card) + ", channel " + std::to_string(channel) +
                         ", handle " + std::to_string(handle) + ")";
    const int status = skiq_read_rx_gain_index_range(card, handle, &minimum, &maximum);
    if (status != 0)
        throw std::runtime_error("skiq_read_rx_gain_index_range" + context + " failed, status " +
                                 std::to_string(status));
    if (minimum > maximum)
        throw std::runtime_error("invalid RX gain index range" + context);

    // Use the same SDK bounds for zero gain, clamping, readback and metadata.
    return {static_cast<double>(minimum), steps_per_db, (maximum - minimum) / steps_per_db};
}

double SoapySidekiq::txGainMaximum(const size_t channel) const
{
    validateChannel(SOAPY_SDR_TX, channel);
    const auto maximum = param.tx_param[channel].atten_quarter_db_max;
    const auto minimum = param.tx_param[channel].atten_quarter_db_min;
    if (minimum > maximum)
        throw std::runtime_error("invalid TX attenuation limits (card " + std::to_string(card) +
                                 ", channel " + std::to_string(channel) + ")");
    return static_cast<double>(maximum - minimum) / 4.0;
}

void SoapySidekiq::validateGainName(const int direction, const size_t channel,
                                    const std::string &name) const
{
    const auto choices = listGains(direction, channel);
    if (std::find(choices.begin(), choices.end(), name) == choices.end())
        throw std::invalid_argument(
            "unknown gain element '" + name + "' (card " + std::to_string(card) + ", direction " +
            std::to_string(direction) + ", channel " + std::to_string(channel) + ")");
}

std::vector<std::string> SoapySidekiq::listGains(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    // Retain the existing element name for client compatibility.
    return {"LNA"};
}

bool SoapySidekiq::hasGainMode(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    return direction == SOAPY_SDR_RX;
}

void SoapySidekiq::setGainMode(const int direction, const size_t channel, const bool automatic)
{
    validateChannel(direction, channel);
    if (direction == SOAPY_SDR_TX)
    {
        if (automatic)
            throw std::invalid_argument("TX automatic gain mode is not supported");
        // Manual-only TX already satisfies a request to disable AGC.
        return;
    }
    const auto handle = getRxHandle(channel);
    const auto mode = automatic ? skiq_rx_gain_auto : skiq_rx_gain_manual;
    const int status = skiq_write_rx_gain_mode(card, handle, mode);
    if (status != 0)
        throw std::runtime_error("skiq_write_rx_gain_mode failed (card " + std::to_string(card) +
                                 ", channel " + std::to_string(channel) + ", handle " +
                                 std::to_string(handle) + ", status " + std::to_string(status) +
                                 ")");
}

bool SoapySidekiq::getGainMode(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    if (direction == SOAPY_SDR_TX)
        return false;
    const auto handle = getRxHandle(channel);
    skiq_rx_gain_t mode = skiq_rx_gain_manual;
    const int status = skiq_read_rx_gain_mode(card, handle, &mode);
    if (status != 0)
        throw std::runtime_error("skiq_read_rx_gain_mode failed (card " + std::to_string(card) +
                                 ", channel " + std::to_string(channel) + ", handle " +
                                 std::to_string(handle) + ", status " + std::to_string(status) +
                                 ")");
    if (mode != skiq_rx_gain_manual && mode != skiq_rx_gain_auto)
        throw std::runtime_error("skiq_read_rx_gain_mode returned an unsupported gain mode");
    return mode == skiq_rx_gain_auto;
}

void SoapySidekiq::setGain(const int direction, const size_t channel, const std::string &name,
                           const double value)
{
    validateGainName(direction, channel, name);
    setGain(direction, channel, value);
}

void SoapySidekiq::setGain(const int direction, const size_t channel, const double value)
{
    validateChannel(direction, channel);
    if (!std::isfinite(value))
        throw std::invalid_argument("setGain requires a finite dB value");
    const auto context =
        " (card " + std::to_string(card) + ", channel " + std::to_string(channel) + ")";
    if (direction == SOAPY_SDR_RX)
    {
        const auto profile = gainProfile(channel);
        const auto handle = getRxHandle(channel);

        // Clamp in the dB domain before multiplication/rounding and narrowing.
        // This also keeps even DBL_MAX requests from overflowing intermediate math.
        const double bounded = std::clamp(value, 0.0, profile.rx_maximum);
        const double index = profile.rx_offset + std::round(bounded * profile.rx_steps_per_db);
        const auto gain_index = static_cast<uint8_t>(
            std::clamp(index, profile.rx_offset,
                       profile.rx_offset + profile.rx_maximum * profile.rx_steps_per_db));

        // Validate profile/range before changing hardware gain mode.
        skiq_rx_gain_t mode = skiq_rx_gain_manual;
        int status = skiq_read_rx_gain_mode(card, handle, &mode);
        if (status != 0)
            throw std::runtime_error("skiq_read_rx_gain_mode" + context + " failed, status " +
                                     std::to_string(status));
        if (mode == skiq_rx_gain_auto)
        {
            status = skiq_write_rx_gain_mode(card, handle, skiq_rx_gain_manual);
            if (status != 0)
                throw std::runtime_error("skiq_write_rx_gain_mode" + context + " failed, status " +
                                         std::to_string(status));
        }
        status = skiq_write_rx_gain(card, handle, gain_index);
        if (status != 0)
            throw std::runtime_error("skiq_write_rx_gain" + context + " failed, status " +
                                     std::to_string(status));
    }
    else
    {
        const auto maximum_gain = txGainMaximum(channel);
        if (value < 0 || value > maximum_gain)
            throw std::invalid_argument("TX gain" + context + " must be between 0 and " +
                                        std::to_string(maximum_gain) + " dB");
        const auto maximum = param.tx_param[channel].atten_quarter_db_max;
        const auto index = static_cast<uint16_t>(maximum - std::round(value * 4.0));
        const int status = skiq_write_tx_attenuation(card, getTxHandle(channel), index);
        if (status != 0)
            throw std::runtime_error("skiq_write_tx_attenuation" + context + " failed, status " +
                                     std::to_string(status));
    }
}

double SoapySidekiq::getGain(const int direction, const size_t channel,
                             const std::string &name) const
{
    validateGainName(direction, channel, name);
    return getGain(direction, channel);
}

double SoapySidekiq::getGain(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    const auto context =
        " (card " + std::to_string(card) + ", channel " + std::to_string(channel) + ")";
    if (direction == SOAPY_SDR_RX)
    {
        const auto profile = gainProfile(channel);
        uint8_t index{};
        const auto handle = getRxHandle(channel);
        const int status = skiq_read_rx_gain(card, handle, &index);
        if (status != 0)
            throw std::runtime_error("skiq_read_rx_gain" + context + " failed, status " +
                                     std::to_string(status));
        if (index < profile.rx_offset ||
            index > profile.rx_offset + profile.rx_maximum * profile.rx_steps_per_db)
            throw std::runtime_error("RX gain readback is outside its index range" + context);
        return (index - profile.rx_offset) / profile.rx_steps_per_db;
    }
    uint16_t index{};
    txGainMaximum(channel); // Validate metadata before hardware access/subtraction.
    const auto maximum = param.tx_param[channel].atten_quarter_db_max;
    const int status = skiq_read_tx_attenuation(card, getTxHandle(channel), &index);
    if (status != 0)
        throw std::runtime_error("skiq_read_tx_attenuation" + context + " failed, status " +
                                 std::to_string(status));
    if (index < param.tx_param[channel].atten_quarter_db_min || index > maximum)
        throw std::runtime_error("TX attenuation readback is outside its limits" + context);
    return static_cast<double>(maximum - index) / 4.0;
}

SoapySDR::Range SoapySidekiq::getGainRange(const int direction, const size_t channel,
                                           const std::string &name) const
{
    validateGainName(direction, channel, name);
    SoapySDR_log(SOAPY_SDR_TRACE, "getGainRange with name");
    return getGainRange(direction, channel);
}

SoapySDR::Range SoapySidekiq::getGainRange(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    if (direction == SOAPY_SDR_RX)
    {
        const auto profile = gainProfile(channel);
        return SoapySDR::Range(0, profile.rx_maximum, 1.0 / profile.rx_steps_per_db);
    }
    return SoapySDR::Range(0, txGainMaximum(channel), 0.25);
}

/*******************************************************************
 * Frequency API
 ******************************************************************/

void SoapySidekiq::setFrequency(const int direction, const size_t channel, const double frequency,
                                const SoapySDR::Kwargs &args)
{
    validateChannel(direction, channel);
    int status = 0;

    SoapySDR_log(SOAPY_SDR_TRACE, "setFrequency");

    if (direction == SOAPY_SDR_RX)
    {
        skiq_rx_hdl_t rx_hdl = getRxHandle(channel);
        const uint64_t requested_frequency =
            soapy_sidekiq::checkedUnsigned<uint64_t>(frequency, "setFrequency RX");

        SoapySDR_logf(SOAPY_SDR_INFO, "Setting rx center freq: %lu", requested_frequency);

        status = skiq_write_rx_LO_freq(this->card, rx_hdl, requested_frequency);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_rx_LO_freq failed, (card %u, frequency "
                          "%lu), status %d",
                          this->card, requested_frequency, status);
            std::ostringstream oss;
            oss << "Failed to set RX LO frequency on card " << unsigned(this->card) << ", channel "
                << channel << " to " << requested_frequency << " Hz (status " << status << ")";
            throw std::runtime_error(oss.str());
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        skiq_tx_hdl_t tx_hdl = getTxHandle(channel);
        const uint64_t requested_frequency =
            soapy_sidekiq::checkedUnsigned<uint64_t>(frequency, "setFrequency TX");

        SoapySDR_logf(SOAPY_SDR_INFO, "Setting tx center freq: %lu", requested_frequency);

        status = skiq_write_tx_LO_freq(this->card, tx_hdl, requested_frequency);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_tx_LO_freq failed, (card %u, frequency "
                          "%lu), status %d",
                          this->card, requested_frequency, status);
            std::ostringstream oss;
            oss << "Failed to set TX LO frequency on card " << unsigned(this->card) << ", channel "
                << channel << " to " << requested_frequency << " Hz (status " << status << ")";
            throw std::runtime_error(oss.str());
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
    validateChannel(direction, channel);
    int status = 0;

    SoapySDR_log(SOAPY_SDR_TRACE, "getFrequency");

    if (direction == SOAPY_SDR_RX)
    {
        uint64_t freq{};
        double tuned_freq = std::numeric_limits<double>::quiet_NaN();
        skiq_rx_hdl_t rx_hdl = getRxHandle(channel);
        status = skiq_read_rx_LO_freq(card, rx_hdl, &freq, &tuned_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_LO_freq (card %u), status %d", card,
                          status);
            throw std::runtime_error("skiq_read_rx_LO_freq failed (card " + std::to_string(card) +
                                     ", channel " + std::to_string(channel) + ", handle " +
                                     std::to_string(rx_hdl) + ", status " + std::to_string(status) +
                                     ")");
        }
        if (!std::isfinite(tuned_freq) || tuned_freq < 0 || tuned_freq >= std::ldexp(1.0, 64))
            throw std::runtime_error("skiq_read_rx_LO_freq returned invalid tuned frequency");
        return tuned_freq;
    }
    else if (direction == SOAPY_SDR_TX)
    {
        uint64_t freq{};
        double tuned_freq = std::numeric_limits<double>::quiet_NaN();
        skiq_tx_hdl_t tx_hdl = getTxHandle(channel);
        status = skiq_read_tx_LO_freq(card, tx_hdl, &freq, &tuned_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_tx_LO_freq (card %u), status %d", card,
                          status);
            throw std::runtime_error("skiq_read_tx_LO_freq failed (card " + std::to_string(card) +
                                     ", channel " + std::to_string(channel) + ", handle " +
                                     std::to_string(tx_hdl) + ", status " + std::to_string(status) +
                                     ")");
        }
        if (!std::isfinite(tuned_freq) || tuned_freq < 0 || tuned_freq >= std::ldexp(1.0, 64))
            throw std::runtime_error("skiq_read_tx_LO_freq returned invalid tuned frequency");
        return tuned_freq;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return 0;
}

SoapySDR::RangeList SoapySidekiq::getFrequencyRange(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    SoapySDR::RangeList results;

    SoapySDR_log(SOAPY_SDR_TRACE, "getFrequencyRange");

    uint64_t max{};
    uint64_t min{};
    int status = 0;

    if (direction == SOAPY_SDR_RX)
    {
        status = skiq_read_rx_LO_freq_range(card, &max, &min);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_LO_freq_range failed,(card %u), status %d",
                          card, status);
            throw std::runtime_error("skiq_read_rx_LO_freq_range failed (card " +
                                     std::to_string(card) + ", channel " + std::to_string(channel) +
                                     ", status " + std::to_string(status) + ")");
        }
        if (min > max)
            throw std::runtime_error("skiq_read_rx_LO_freq_range returned an inverted range");
        results.push_back(SoapySDR::Range(min, max));
    }
    else if (direction == SOAPY_SDR_TX)
    {
        status = skiq_read_tx_LO_freq_range(card, &max, &min);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_tx_LO_freq_range failed (card %u), status %d",
                          card, status);
            throw std::runtime_error("skiq_read_tx_LO_freq_range failed (card " +
                                     std::to_string(card) + ", channel " + std::to_string(channel) +
                                     ", status " + std::to_string(status) + ")");
        }
        if (min > max)
            throw std::runtime_error("skiq_read_tx_LO_freq_range returned an inverted range");
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

void SoapySidekiq::setSampleRate(const int direction, const size_t channel, const double rate)
{
    validateChannel(direction, channel);
    const auto requested = soapy_sidekiq::checkedUnsigned<uint32_t>(rate, "setSampleRate");
    if (requested == 0)
        throw std::invalid_argument("setSampleRate requires at least one sample per second");
    const auto previous = readRateAndBandwidth(direction, channel);
    // Preserve the current hardware bandwidth, clamped to the new sample rate.
    applyRateAndBandwidth(direction, channel, requested,
                          std::min(previous.actual_bandwidth, requested));
}

double SoapySidekiq::getSampleRate(const int direction, const size_t channel) const
{
    return readRateAndBandwidth(direction, channel).actual_rate;
}

SoapySDR::RangeList SoapySidekiq::getSampleRateRange(const int direction,
                                                     const size_t channel) const
{
    validateChannel(direction, channel);
    uint32_t minimum{}, maximum{};
    const std::string context =
        " (card " + std::to_string(card) + ", channel " + std::to_string(channel) + ")";
    int status = skiq_read_min_sample_rate(card, &minimum);
    if (status != 0)
        throw std::runtime_error("skiq_read_min_sample_rate" + context + " failed, status " +
                                 std::to_string(status));
    status = skiq_read_max_sample_rate(card, &maximum);
    if (status != 0)
        throw std::runtime_error("skiq_read_max_sample_rate" + context + " failed, status " +
                                 std::to_string(status));
    if (minimum == 0 || minimum > maximum)
        throw std::runtime_error("invalid SDK sample-rate bounds" + context);
    return {SoapySDR::Range(minimum, maximum)};
}

void SoapySidekiq::initializeRateCaches()
{
    // A constructor may encounter partial SDK outputs or a failure on a later
    // handle. Validate into local storage before exposing any initial snapshot.
    std::vector<uint32_t> rx_rates(num_rx_channels), rx_widths(num_rx_channels);
    std::vector<uint32_t> tx_rates(num_tx_channels), tx_widths(num_tx_channels);
    for (size_t channel = 0; channel < num_rx_channels; ++channel)
    {
        const auto observed = readRateAndBandwidth(SOAPY_SDR_RX, channel);
        rx_rates[channel] = static_cast<uint32_t>(observed.actual_rate);
        rx_widths[channel] = observed.actual_bandwidth;
    }
    for (size_t channel = 0; channel < num_tx_channels; ++channel)
    {
        const auto observed = readRateAndBandwidth(SOAPY_SDR_TX, channel);
        tx_rates[channel] = static_cast<uint32_t>(observed.actual_rate);
        tx_widths[channel] = observed.actual_bandwidth;
    }
    rx_sample_rates.swap(rx_rates);
    rx_bandwidths.swap(rx_widths);
    tx_sample_rates.swap(tx_rates);
    tx_bandwidths.swap(tx_widths);
}

SoapySidekiq::RateAndBandwidth SoapySidekiq::readRateAndBandwidth(const int direction,
                                                                  const size_t channel) const
{
    validateChannel(direction, channel);
    uint32_t configured_rate{}, configured_bw{}, actual_bw{};
    double actual_rate = std::numeric_limits<double>::quiet_NaN();
    const auto operation = std::string(direction == SOAPY_SDR_RX ? "RX" : "TX") +
                           " rate/bandwidth readback (card " + std::to_string(card) + ", channel " +
                           std::to_string(channel) + ")";
    const int status =
        direction == SOAPY_SDR_RX
            ? skiq_read_rx_sample_rate_and_bandwidth(card, getRxHandle(channel), &configured_rate,
                                                     &actual_rate, &configured_bw, &actual_bw)
            : skiq_read_tx_sample_rate_and_bandwidth(card, getTxHandle(channel), &configured_rate,
                                                     &actual_rate, &configured_bw, &actual_bw);
    if (status != 0)
        throw std::runtime_error(operation + " failed, status " + std::to_string(status));
    if (configured_rate == 0 || !std::isfinite(actual_rate) || actual_rate < 1 ||
        actual_rate >= std::ldexp(1.0, 32))
        throw std::runtime_error(operation + " returned an invalid sample rate");
    // Zero may request automatic bandwidth selection, but it is not a usable
    // actual filter width. The zero initializer also detects omitted output.
    if (actual_bw == 0)
        throw std::runtime_error(operation + " returned missing or zero actual bandwidth");
    return {configured_rate, actual_rate, actual_bw};
}

void SoapySidekiq::applyRateAndBandwidth(const int direction, const size_t channel,
                                         const uint32_t rate, const uint32_t bandwidth)
{
    validateChannel(direction, channel);
    auto &cached_rate = (direction == SOAPY_SDR_RX ? rx_sample_rates : tx_sample_rates).at(channel);
    auto &cached_bw = (direction == SOAPY_SDR_RX ? rx_bandwidths : tx_bandwidths).at(channel);
    const int status =
        direction == SOAPY_SDR_RX
            ? skiq_write_rx_sample_rate_and_bandwidth(card, getRxHandle(channel), rate, bandwidth)
            : skiq_write_tx_sample_rate_and_bandwidth(card, getTxHandle(channel), rate, bandwidth);
    if (status != 0)
        throw std::runtime_error(std::string(direction == SOAPY_SDR_RX ? "RX" : "TX") +
                                 " rate/bandwidth write failed (card " + std::to_string(card) +
                                 ", channel " + std::to_string(channel) + ", rate " +
                                 std::to_string(rate) + ", bandwidth " + std::to_string(bandwidth) +
                                 ", status " + std::to_string(status) + ")");

    // The SDK may write some outputs before reporting failure. Keep every
    // output local and commit both cache entries only after validated success.
    // This preserves last-known cache state; it cannot undo a hardware write.
    const auto observed = readRateAndBandwidth(direction, channel);
    cached_rate = static_cast<uint32_t>(observed.actual_rate);
    cached_bw = observed.actual_bandwidth;
}

void SoapySidekiq::setBandwidth(const int direction, const size_t channel, const double bw)
{
    validateChannel(direction, channel);
    const auto requested = soapy_sidekiq::checkedUnsigned<uint32_t>(bw, "setBandwidth");
    // Read the hardware rate: RX/TX clocks can be coupled and a cached rate
    // may have become stale after a change on another handle/direction.
    const auto previous = readRateAndBandwidth(direction, channel);
    applyRateAndBandwidth(direction, channel, previous.configured_rate, requested);
}

/***********************************************************
 * Some open source call listSampleRates and want a list of available
 * sample rates.  For AD9361 cards, these have contiguous sample rates
 * available.
 * For other cards, there are profiles and there are only specific rates available
 * For now we will just give a list of min-max, with steps of 250k Hz.
 */
std::vector<double> SoapySidekiq::listSampleRates(const int direction, const size_t channel) const
{
    const auto bounds = getSampleRateRange(direction, channel).front();
    const auto minimum = static_cast<uint64_t>(bounds.minimum());
    const auto maximum = static_cast<uint64_t>(bounds.maximum());
    std::vector<double> rates;
    // Keep the legacy compatibility grid. A wider counter prevents uint32
    // wraparound at a large SDK maximum; this does not enumerate RF profiles.
    for (uint64_t rate = minimum; rate <= maximum; rate += 250000)
        rates.push_back(static_cast<double>(rate));
    return rates;
}

double SoapySidekiq::getBandwidth(const int direction, const size_t channel) const
{
    return readRateAndBandwidth(direction, channel).actual_bandwidth;
}
