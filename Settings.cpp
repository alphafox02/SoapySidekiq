/**
 * @file Settings.cpp
 * @brief Implements Sidekiq device settings, callbacks, and utility operations.
 */

#include "SoapySidekiq.hpp"
#include <SoapySidekiq/DeviceOptions.hpp>
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySidekiq/SidekiqRxBackend.hpp>
#include <SoapySidekiq/SampleScale.hpp>
#include <SoapySidekiq/StringUtils.hpp>
#include <SoapySDR/Formats.hpp>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cinttypes>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <string>
#include <sidekiq_types.h>

// Keep the hardware-independent validation contract tied to the SDK in use.
static_assert(soapy_sidekiq::maxTxBlockSize == SKIQ_MAX_TX_BLOCK_SIZE_IN_WORDS);
static_assert(soapy_sidekiq::txHeaderWords == SKIQ_TX_HEADER_SIZE_IN_WORDS);
static_assert(soapy_sidekiq::txPacketIncrementWords == SKIQ_TX_PACKET_SIZE_INCREMENT_IN_WORDS);

/**
 * Forward an SDK diagnostic to SoapySDR without taking ownership of its text.
 * @param priority SDK log priority, mapped to the corresponding Soapy level.
 * @param message Borrowed, null-terminated SDK text; null is treated as empty.
 *
 * Remove at most one trailing LF and then one CR, preserving the historical
 * line-ending behavior. Diagnostics must not propagate C++ exceptions through
 * the SDK's C callback boundary, even if allocation or a log sink fails.
 */
void logging_handler(int32_t priority, const char *message) noexcept
{
    try
    {
        std::string text(message != nullptr ? message : "");
        if (!text.empty() && text.back() == '\n')
        {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '\r')
        {
            text.pop_back();
        }

        switch (priority)
        {
        case SKIQ_LOG_DEBUG:
            SoapySDR_logf(SOAPY_SDR_DEBUG, "epiq-log: %s", text.c_str());
            break;
        case SKIQ_LOG_INFO:
            SoapySDR_logf(SOAPY_SDR_INFO, "epiq-log: %s", text.c_str());
            break;
        case SKIQ_LOG_WARNING:
            SoapySDR_logf(SOAPY_SDR_WARNING, "epiq-log: %s", text.c_str());
            break;
        case SKIQ_LOG_ERROR:
            SoapySDR_logf(SOAPY_SDR_ERROR, "epiq-log: %s", text.c_str());
            break;
        default:
            SoapySDR_logf(SOAPY_SDR_TRACE, "epiq-log undefined %s", text.c_str());
        }
    }
    catch (...)
    {
        // Drop a failed diagnostic. Retrying the sink could recurse or throw
        // again while the SDK is already reporting an error.
    }
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
    if (txIndex >= DEFAULT_NUM_BUFFERS || !p_tx_status)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "TX completion received invalid buffer index %u", txIndex);
        return;
    }
    // update the in use status of the packet just completed
    bool unexpected_status;
    {
        std::lock_guard<std::mutex> lock(tx_buf_mutex);
        unexpected_status = p_tx_status[txIndex] != 1;
        p_tx_status[txIndex] = 0;
        ++complete_count;
    }
    tx_buf_cv.notify_all();
    // Publish completion before diagnostics, and never call a log sink with
    // the status mutex held: user logging code may throw or re-enter.
    if (unexpected_status)
        SoapySDR_logf(SOAPY_SDR_ERROR, "TX completion received a buffer that was not reserved");
}


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

void SoapySidekiq::validateChannel(const int direction, const size_t channel) const
{
    const auto count = getNumChannels(direction);
    if (channel >= count)
    {
        throw std::out_of_range(std::string(direction == SOAPY_SDR_RX ? "RX" : "TX") + " channel " +
                                std::to_string(channel) + " is unavailable; device has " +
                                std::to_string(count) + " channel(s)");
    }
}

skiq_rx_hdl_t SoapySidekiq::getRxHandle(const size_t channel) const
{
    validateChannel(SOAPY_SDR_RX, channel);
    return this->param.rx_param[channel].handle;
}

skiq_tx_hdl_t SoapySidekiq::getTxHandle(const size_t channel) const
{
    validateChannel(SOAPY_SDR_TX, channel);
    return this->param.tx_param[channel].handle;
}

skiq_rx_hdl_t SoapySidekiq::getFirstRxHandle(void) const
{
    return getRxHandle(0);
}

skiq_tx_hdl_t SoapySidekiq::getFirstTxHandle(void) const
{
    return getTxHandle(0);
}

// Acquire constructor callback resources as one transaction. No stream can be
// active yet, so failed registration can be unwound before SDK initialization
// is released by the constructor's outer rollback guard.
void SoapySidekiq::initializeTxCallbacks()
{
    registerInstance(card, this);
    soapy_sidekiq::ScopeExit registry_rollback([this]() noexcept
                                               { unregisterInstance(card, this); });

    const int status =
        skiq_register_tx_complete_callback(card, &SoapySidekiq::tx_complete_callback);
    if (status != 0)
    {
        throw std::runtime_error("skiq_register_tx_complete_callback failed (card " +
                                 std::to_string(card) + ", status " + std::to_string(status) + ")");
    }
    soapy_sidekiq::ScopeExit callback_rollback(
        [this]() noexcept { skiq_register_tx_complete_callback(card, nullptr); });

    // A user-installed log sink may throw. Keep rollback armed through the
    // final potentially throwing operation in the device constructor.
    SoapySDR_logf(SOAPY_SDR_TRACE, "leaving constructor", card);
    callback_rollback.release();
    registry_rollback.release();
}

// Constructor
SoapySidekiq::SoapySidekiq(const SoapySDR::Kwargs &args)
{
    int status = 0;
    uint8_t channels = 0;
    skiq_iq_order_t iq_order;
    // Reject all malformed constructor input before registering or opening SDK state.
    const auto options = soapy_sidekiq::parseDeviceOptions(args);

    /* Register our own logging function before initializing the library */
    skiq_register_logging( logging_handler );

    SoapySDR_logf(SOAPY_SDR_TRACE, "in constructor", card);

    /* We need to set some default parameters in case the user does not */
    rxUseShort  = true;
    txUseShort  = true;
    iq_swap   = true;
    counter   = false;
    card      = 0;
    rx_block_size_in_words = 0;
    rx_block_size_in_bytes = 0;
    rx_payload_size_in_words = 1018;
    rx_payload_size_in_bytes = 0;
    rfTimeSource = true;
    timetype = "rf_timestamp";
    complete_count = 0;

    card = options.card;
    current_tx_block_size = options.tx_block_size;
    SoapySDR_logf(SOAPY_SDR_INFO, "TX block size set to %u", current_tx_block_size);

    skiq_xport_type_t type  = skiq_xport_type_auto;
    skiq_xport_init_level_t level = skiq_xport_init_level_full;

    SoapySDR_logf(SOAPY_SDR_INFO, "Sidekiq opening card %u", card);

    /* init sidekiq */
    status = skiq_init(type, level, &card, 1);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_init failed (card %u), status %d",
                      card, status);
        throw std::runtime_error("skiq_init failed (card " + std::to_string(card) + ", status " +
                                 std::to_string(status) + ")");
    }

    // A throwing constructor never runs the device destructor. Only arm this
    // rollback after this call successfully acquires SDK initialization.
    soapy_sidekiq::ScopeExit rollback(
        [this]() noexcept
        {
            p_tx_status.reset();
            skiq_exit();
        });
    // The backend owns no hardware resource; the session owns its worker.
    rx_backend = std::make_unique<soapy_sidekiq::SidekiqRxBackend>(card);
    rx_session = std::make_unique<soapy_sidekiq::RxStreamSession>(*rx_backend);

    if (options.topology.has_value())
    {
        if (skiq_is_topology_supported(card))
        {
            status = skiq_apply_topology(card, *options.topology);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_apply_topology failed (card %u), status %d",
                              card, status);
                throw std::runtime_error("skiq_apply_topology failed (card " +
                                         std::to_string(card) + ", topology " +
                                         std::to_string(static_cast<int>(*options.topology)) +
                                         ", status " + std::to_string(status) + ")");
            }
        }
	else
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "Topology not supported on card %u; ignoring request",
                          card, status);
        }
    }

    status = skiq_read_parameters(card, &this->param);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_parameters failed, card %u, status %d",
                      card, status);
        throw std::runtime_error("skiq_read_parameters failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
    }

    status = skiq_read_sys_timestamp_freq(card, &sys_freq);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_sys_timestamp_freq failed (card %u), status %d",
                      card, status);
        throw std::runtime_error("skiq_read_sys_timestamp_freq failed (card " +
                                 std::to_string(card) + ", status " + std::to_string(status) + ")");
    }

    part = param.card_param.part_type;
    part_str = skiq_part_string(part);

    SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, part type is %s", card, part_str.c_str());

    /* set iq order to iq instead of qi */
    if (iq_swap)
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
        throw std::runtime_error("skiq_write_iq_order_mode failed (card " + std::to_string(card) +
                                 ", IQ order " + std::to_string(static_cast<int>(iq_order)) +
                                 ", status " + std::to_string(status) + ")");
    }

    status = skiq_read_num_rx_chans(card, &channels);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_num_rx_chans failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("skiq_read_num_rx_chans failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
    }
    num_rx_channels = channels;

    for (size_t chan = 0; chan < num_rx_channels; chan++)
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "Soapy RX channel %zu maps to Sidekiq rx handle %u",
                      chan, getRxHandle(chan));
    }

    status = skiq_read_num_tx_chans(card, &channels);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_num_tx_chans failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("skiq_read_num_tx_chans failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
    }
    num_tx_channels = channels;
    for (size_t chan = 0; chan < num_tx_channels; chan++)
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "Soapy TX channel %zu maps to Sidekiq tx handle %u",
                      chan, getTxHandle(chan));
    }

    uint8_t tmp_resolution = 0;

    /* Every card can have a different iq resolution.  */
    status = skiq_read_rx_iq_resolution(card, &tmp_resolution);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_iq_resolution failed, "
                      "card: %u status: %d",
                      card, status);
        throw std::runtime_error("skiq_read_rx_iq_resolution failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
    }

    this->resolution = tmp_resolution;
    this->maxValue = soapy_sidekiq::sampleFullScale(tmp_resolution);

    SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, card resolution: %u bits, max ADC value: %u", card,
                  this->resolution, static_cast<uint32_t>(this->maxValue));

    // The transport stores signed 16-bit components, but DAC width is product-specific.
    uint8_t tx_resolution = 0;
    status = skiq_read_tx_iq_resolution(card, &tx_resolution);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_tx_iq_resolution failed, card: %u status: %d",
                      card, status);
        throw std::runtime_error("skiq_read_tx_iq_resolution failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
    }
    // The SDK manual lists these products as upper-14-bit TX. Their low two
    // host bits are discarded, so DAC resolution is not the host amplitude width.
    const auto tx_alignment = (part == skiq_x2 || part == skiq_x4 || part == skiq_x40)
        ? soapy_sidekiq::TxSampleAlignment::upper_bits
        : soapy_sidekiq::TxSampleAlignment::right_justified;
    this->txMaxValue = soapy_sidekiq::txSampleFullScale(tx_resolution, tx_alignment);
    SoapySDR_logf(SOAPY_SDR_INFO,
                  "card: %u, TX DAC resolution: %u bits, host full scale: %u",
                  card, tx_resolution, static_cast<uint32_t>(this->txMaxValue));

    status = skiq_write_chan_mode(card, skiq_chan_mode_single);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_chan_mode failed, card %u, status %d",
                      card, status);
        throw std::runtime_error("skiq_write_chan_mode failed (card " + std::to_string(card) +
                                 ", channel mode single" + ", status " + std::to_string(status) +
                                 ")");
    }
    SoapySDR_logf(SOAPY_SDR_TRACE, "channel mode set to single");

    // Apply the same checked SDK readback contract used by RF configuration.
    initializeRateCaches();

    // Source setters require initialized hardware, so apply validated names last.
    if (options.clock_source.has_value())
    {
        setClockSource(*options.clock_source);
    }

    if (options.time_source.has_value())
    {
        setTimeSource(*options.time_source);
    }

    // Value-initialize every entry before making storage visible to callbacks.
    // The member owner also releases it if later constructor work throws.
    p_tx_status = std::make_unique<int32_t[]>(DEFAULT_NUM_BUFFERS);

    initializeTxCallbacks();
    rollback.release();
}

// Destructor
SoapySidekiq::~SoapySidekiq(void)
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "In destructor", card);

    // Stop and join RX before invalidating callbacks or exiting the SDK.
    if (rx_session != nullptr)
    {
        rx_session->shutdown();
    }
    rx_session.reset();
    rx_backend.reset();
    delete active_rx_stream;
    active_rx_stream = nullptr;

    unregisterInstance(card, this);
    skiq_register_tx_complete_callback(card, nullptr);

    p_tx_status.reset();

    skiq_exit();
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
    args["rx_channels"]   = std::to_string(num_rx_channels);
    args["tx_channels"]   = std::to_string(num_tx_channels);

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
        return num_rx_channels;
    }
    else if (dir == SOAPY_SDR_TX)
    {
        return num_tx_channels;
    }
    else
    {
        throw std::invalid_argument("invalid stream direction " + std::to_string(dir));
    }

}

SoapySDR::Kwargs SoapySidekiq::getChannelInfo(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    SoapySDR::Kwargs info;
    info["card"] = std::to_string(card);
    info["card_type"] = part_str;
    info["sidekiq_channel"] = std::to_string(channel);

    std::string name;
    if (direction == SOAPY_SDR_RX)
    {
        const auto handle = getRxHandle(channel);
        info["sidekiq_handle_id"] = std::to_string(handle);
        switch (handle)
        {
        case skiq_rx_hdl_A1:
            name = "A1";
            break;
        case skiq_rx_hdl_A2:
            name = "A2";
            break;
        case skiq_rx_hdl_B1:
            name = "B1";
            break;
        case skiq_rx_hdl_B2:
            name = "B2";
            break;
        case skiq_rx_hdl_C1:
            name = "C1";
            break;
        case skiq_rx_hdl_D1:
            name = "D1";
            break;
        default:
            name = std::to_string(handle);
            break;
        }
    }
    else
    {
        const auto handle = getTxHandle(channel);
        info["sidekiq_handle_id"] = std::to_string(handle);
        switch (handle)
        {
        case skiq_tx_hdl_A1:
            name = "A1";
            break;
        case skiq_tx_hdl_A2:
            name = "A2";
            break;
        case skiq_tx_hdl_B1:
            name = "B1";
            break;
        case skiq_tx_hdl_B2:
            name = "B2";
            break;
        default:
            name = std::to_string(handle);
            break;
        }
    }
    info["sidekiq_handle"] = name;
    info["label"] = std::string(direction == SOAPY_SDR_RX ? "RX " : "TX ") + name;
    return info;
}

/*******************************************************************
 * Antenna API
 ******************************************************************/

/*******************************************************************
 * Settings API
 ******************************************************************/

SoapySDR::ArgInfoList SoapySidekiq::getSettingInfo(void) const
{
    SoapySDR::ArgInfoList setArgs;
    SoapySDR_logf(SOAPY_SDR_TRACE, "getSettingInfo");

    SoapySDR::ArgInfo settingArg;

    settingArg.key         = "iq_swap";
    settingArg.value = iq_swap ? "true" : "false";
    settingArg.name        = "I/Q Swap";
    settingArg.description = "Set I then Q format";
    settingArg.type        = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(settingArg);

    settingArg.key         = "counter";
    settingArg.value = counter ? "true" : "false";
    settingArg.name        = "counter";
    settingArg.description = "FPGA creates RAMP samples";
    settingArg.type        = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(settingArg);

    settingArg.key         = "sys_clock_freq";
    settingArg.value = std::to_string(sys_freq);
    settingArg.name        = "sys_clock_freq";
    settingArg.description = "Read-only frequency of the system timestamp clock";
    settingArg.type        = SoapySDR::ArgInfo::STRING;
    setArgs.push_back(settingArg);

    settingArg.options.clear();
    settingArg.key         = "full_scale";
    settingArg.value       = std::to_string(static_cast<uint32_t>(this->maxValue));
    settingArg.name        = "full_scale";
    settingArg.description = "Read-only device-specific full-scale integer sample value";
    settingArg.type        = SoapySDR::ArgInfo::STRING;
    setArgs.push_back(settingArg);

    settingArg.key         = "timetype";
    settingArg.value = timetype;
    settingArg.name        = "timetype";
    settingArg.description = "The type of timestamp to return in readStream";
    settingArg.type        = SoapySDR::ArgInfo::STRING;
    settingArg.options.push_back("rf_timestamp");
    settingArg.options.push_back("sys_timestamp");
    setArgs.push_back(settingArg);

    return setArgs;
}

namespace
{
/** Parse documented boolean forms before any device or cached-state changes. */
bool parseSettingBool(const std::string &key, const std::string &value)
{
    if (equalsIgnoreCase(value, "true") || value == "1")
        return true;
    if (equalsIgnoreCase(value, "false") || value == "0")
        return false;
    throw std::invalid_argument("Setting " + key + " requires true, false, 1, or 0");
}
} // namespace

void SoapySidekiq::writeSetting(const std::string &key, const std::string &value)
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "writeSetting");
    if (equalsIgnoreCase(key, "iq_swap"))
    {
        const bool requested = parseSettingBool(key, value);
        const int status =
            skiq_write_iq_order_mode(card, requested ? skiq_iq_order_iq : skiq_iq_order_qi);
        if (status != 0)
            throw std::runtime_error("skiq_write_iq_order_mode failed: " + std::to_string(status));
        iq_swap = requested;
    }
    else if (equalsIgnoreCase(key, "counter"))
    {
        const bool requested = parseSettingBool(key, value);
        if (num_rx_channels == 0)
            throw std::runtime_error("Counter setting requires an RX channel");
        const auto source = requested ? skiq_data_src_counter : skiq_data_src_iq;
        // Snapshot every mapped handle before writes; allocate rollback storage first.
        std::vector<skiq_rx_hdl_t> handles;
        std::vector<skiq_data_src_t> previous;
        handles.reserve(num_rx_channels);
        previous.reserve(num_rx_channels);
        for (size_t channel = 0; channel < num_rx_channels; ++channel)
        {
            const auto handle = getRxHandle(channel);
            skiq_data_src_t old_source = skiq_data_src_iq;
            const int status = skiq_read_rx_data_src(card, handle, &old_source);
            if (status != 0)
                throw std::runtime_error("skiq_read_rx_data_src failed on channel " +
                                         std::to_string(channel) + ": " + std::to_string(status));
            if (old_source != skiq_data_src_iq && old_source != skiq_data_src_counter)
                throw std::runtime_error("Invalid RX data source on channel " +
                                         std::to_string(channel));
            handles.push_back(handle);
            previous.push_back(old_source);
        }
        for (size_t channel = 0; channel < handles.size(); ++channel)
        {
            const int status = skiq_write_rx_data_src(card, handles[channel], source);
            if (status != 0)
            {
                // Include the failed write: an SDK error can follow a partial update.
                // Finish all compensation attempts before constructing the diagnostic.
                int rollback_status = 0;
                for (size_t attempted = channel + 1; attempted > 0; --attempted)
                {
                    const int restored = skiq_write_rx_data_src(card, handles[attempted - 1],
                                                                previous[attempted - 1]);
                    if (restored != 0 && rollback_status == 0)
                        rollback_status = restored;
                }
                std::string message = "skiq_write_rx_data_src failed on channel " +
                                      std::to_string(channel) + ": " + std::to_string(status);
                if (rollback_status != 0)
                    message +=
                        "; RX data-source rollback failed: " + std::to_string(rollback_status);
                throw std::runtime_error(message);
            }
        }
        counter = requested;
    }
    else if (equalsIgnoreCase(key, "timetype"))
    {
        if (value != "rf_timestamp" && value != "sys_timestamp")
            throw std::invalid_argument("Invalid timetype: " + value);
        timetype = value;
        rfTimeSource = value == "rf_timestamp";
    }
    else if (equalsIgnoreCase(key, "sys_clock_freq") || equalsIgnoreCase(key, "full_scale") ||
             equalsIgnoreCase(key, "overload") || equalsIgnoreCase(key, "cal_offset"))
        throw std::invalid_argument("Read-only setting: " + key);
    else
        throw std::invalid_argument("Unknown setting: " + key);
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
        return std::to_string(static_cast<uint64_t>(this->sys_freq));
    }
    else if (equalsIgnoreCase(key, "full_scale"))
    {
        return std::to_string(static_cast<uint32_t>(this->maxValue));
    }
    else if (equalsIgnoreCase(key, "overload"))
    {
        uint8_t overload_state = 0;
        int32_t status = skiq_read_rx_overload_state(this->card,
                                                     getFirstRxHandle(),
                                                     &overload_state);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_overload_state failed, card: %u status: %d",
                          this->card, status);
            throw std::runtime_error("skiq_read_rx_overload_state failed (card " +
                                     std::to_string(card) + ", channel 0, handle " +
                                     std::to_string(getFirstRxHandle()) + ", status " +
                                     std::to_string(status) + ")");
        }
        return std::to_string(overload_state);
    }
    else if (equalsIgnoreCase(key, "cal_offset"))
    {
        double cal_offset = 0;
        int32_t status = skiq_read_rx_cal_offset(this->card,
                                                     getFirstRxHandle(),
                                                     &cal_offset);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_cal_offset failed, card: %u status: %d",
                          this->card, status);
            throw std::runtime_error("skiq_read_rx_cal_offset failed (card " +
                                     std::to_string(card) + ", channel 0, handle " +
                                     std::to_string(getFirstRxHandle()) + ", status " +
                                     std::to_string(status) + ")");
        }
        return std::to_string(cal_offset);
    }
    else
    {
        throw std::invalid_argument("Unknown setting: " + key);
    }
    return "";
}
