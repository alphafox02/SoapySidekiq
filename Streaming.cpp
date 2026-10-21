/**
 * @file Streaming.cpp
 * @brief Implements SoapySDR stream setup, lifecycle, receive, and transmit I/O.
 */

#include <cstring>
#include <unistd.h>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <new>
#include <chrono>
#include <limits>

#include "SoapySidekiq.hpp"
#include <SoapySDR/Formats.hpp>
#include <SoapySidekiq/SampleConversion.hpp>
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySidekiq/StreamConfiguration.hpp>
#include <SoapySidekiq/TimestampConversion.hpp>
#include <sidekiq_types.h>

namespace
{
std::mutex g_instance_registry_mutex;
std::unordered_map<uint8_t, SoapySidekiq *> g_instance_registry;

/** Translate a SoapySDR direction constant or reject an unknown value. */
soapy_sidekiq::StreamDirection streamDirection(const int direction)
{
    // Reject unknown Soapy constants before selecting RX or TX device state.
    if (direction == SOAPY_SDR_RX)
    {
        return soapy_sidekiq::StreamDirection::rx;
    }
    if (direction == SOAPY_SDR_TX)
    {
        return soapy_sidekiq::StreamDirection::tx;
    }
    throw std::invalid_argument("invalid stream direction " + std::to_string(direction));
}
}

SoapySidekiq::StreamHandle *SoapySidekiq::findStreamHandle(SoapySDR::Stream *stream) const noexcept
{
    // Compare opaque tokens before dereferencing: null, foreign, and closed
    // handles need not point to accessible memory.
    if (stream == nullptr)
        return nullptr;
    if (stream == reinterpret_cast<SoapySDR::Stream *>(active_rx_stream))
        return active_rx_stream;
    if (stream == reinterpret_cast<SoapySDR::Stream *>(active_tx_stream))
        return active_tx_stream;
    return nullptr;
}

void SoapySidekiq::registerInstance(uint8_t card, SoapySidekiq *instance)
{
    std::lock_guard<std::mutex> lock(g_instance_registry_mutex);
    g_instance_registry[card] = instance;
}

void SoapySidekiq::unregisterInstance(uint8_t card, SoapySidekiq *instance)
{
    std::lock_guard<std::mutex> lock(g_instance_registry_mutex);
    const auto it = g_instance_registry.find(card);
    if (it != g_instance_registry.end() && it->second == instance)
    {
        g_instance_registry.erase(it);
    }
}

SoapySidekiq *SoapySidekiq::getInstanceForCard(uint8_t card)
{
    std::lock_guard<std::mutex> lock(g_instance_registry_mutex);
    const auto it = g_instance_registry.find(card);
    return (it != g_instance_registry.end()) ? it->second : nullptr;
}

void SoapySidekiq::tx_complete_callback(int32_t status, skiq_tx_block_t *p_data,
                                        void *p_user) noexcept
{
    // Completion owns the accepted submission's context, including on every
    // diagnostic/exception path. Never unwind through the SDK's C boundary.
    std::unique_ptr<passedStruct> instance(static_cast<passedStruct *>(p_user));
    try
    {
        if (instance == nullptr || instance->classAddr == nullptr)
        {
            SoapySDR_log(SOAPY_SDR_ERROR, "tx_complete callback received invalid user data");
            return;
        }

        instance->classAddr->tx_complete(status, p_data, instance->txIndex);
    }
    catch (...)
    {
        // A failing log sink must not escape or be retried from this callback.
    }
}

long long SoapySidekiq::convert_timestamp_to_nanos(
        const uint64_t timestamp, const uint64_t timestamp_freq) const
{
    // Central helper provides exact arithmetic plus zero/overflow validation.
    return soapy_sidekiq::ticksToNanoseconds(timestamp, timestamp_freq);
}


std::vector<std::string> SoapySidekiq::getStreamFormats(
    const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    SoapySDR_logf(SOAPY_SDR_TRACE, "getStreamFormats");

    std::vector<std::string> formats;

    formats.push_back(SOAPY_SDR_CS16);
    formats.push_back(SOAPY_SDR_CF32);

    return formats;
}

std::string SoapySidekiq::getNativeStreamFormat(const int    direction,
                                                const size_t channel,
                                                double &     fullScale) const
{
    validateChannel(direction, channel);
    SoapySDR_logf(SOAPY_SDR_TRACE, "getNativeStreamFormat");

    const auto stream_direction = streamDirection(direction);
    fullScale = stream_direction == soapy_sidekiq::StreamDirection::rx
        ? this->maxValue : this->txMaxValue;

    return "CS16";
}

SoapySDR::ArgInfoList SoapySidekiq::getStreamArgsInfo(
            const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    SoapySDR::ArgInfoList streamArgs;

    SoapySDR::ArgInfo bufflenArg;
    bufflenArg.key = "bufflen";

    SoapySDR_logf(SOAPY_SDR_TRACE, "getStreamArgsInfo");

    if (direction == SOAPY_SDR_RX)
    {
        bufflenArg.name        = "Buffer Sample Count";
        bufflenArg.description = "Number of IQ samples per buffer.";
        bufflenArg.units       = "(int16_t * 2) samples";
        bufflenArg.type        = SoapySDR::ArgInfo::INT;
        bufflenArg.value       = std::to_string(rx_payload_size_in_words);

        streamArgs.push_back(bufflenArg);
    }
    else
    {
        bufflenArg.name        = "Buffer Sample Count";
        bufflenArg.description = "Number of IQ samples per buffer.";
        bufflenArg.units       = "(int16_t * 2) samples";
        bufflenArg.value       = std::to_string(current_tx_block_size);
        bufflenArg.type        = SoapySDR::ArgInfo::INT;

        streamArgs.push_back(bufflenArg);
    }

    return streamArgs;
}

void SoapySidekiq::stopTxWrites()
{
    {
        std::scoped_lock lock(tx_mutex, tx_buf_mutex);
        tx_writes_stopping = true;
    }
    tx_cv.notify_all();
    tx_buf_cv.notify_all();
    std::unique_lock<std::mutex> lock(tx_buf_mutex);
    tx_buf_cv.wait(lock,
                   [this] { return active_tx_writers == 0 && active_tx_status_readers == 0; });
}

void SoapySidekiq::tx_streaming_start(skiq_tx_hdl_t tx_handle)
{
    SoapySDR_log(SOAPY_SDR_TRACE, "entering tx_streaming_start");
    const int status = skiq_start_tx_streaming_on_1pps(card, tx_handle, 0);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_start_tx_streaming_on_1pps failed, (card %u) status %d",
                card, status);
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "TX start streaming on 1pps completed");
    }

    {
        std::scoped_lock lock(tx_mutex, tx_status_mutex);
        if (status == 0)
            tx_underruns = 0; // SDK resets its counter on start.
        tx_start_status = status;
        tx_start_finished = true;
    }
    tx_cv.notify_all();
}
/*******************************************************************
 * Stream API
 ******************************************************************/

SoapySDR::Stream *SoapySidekiq::setupStream(const int direction,
                                            const std::string &format,
                                            const std::vector<size_t> &channels,
                                            const SoapySDR::Kwargs &args)
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "setupStream");

    const auto stream_direction = streamDirection(direction);
    const bool already_configured = stream_direction == soapy_sidekiq::StreamDirection::rx
        ? active_rx_stream != nullptr
        : active_tx_stream != nullptr;
    const std::size_t available_channels =
        stream_direction == soapy_sidekiq::StreamDirection::rx
            ? num_rx_channels
            : num_tx_channels;
    const auto request = soapy_sidekiq::validateStreamRequest(
        stream_direction, format, channels, available_channels, already_configured);
    const std::size_t channel = request.channel;

    if (direction == SOAPY_SDR_RX)
    {
        if (active_rx_stream != nullptr)
        {
            throw std::runtime_error("only one RX stream per device is currently supported");
        }

        //  check the channel configuration
        if (channels.size() > 1)
        {
            throw std::runtime_error("multi-channel RX streams are not currently supported");
        }
        skiq_rx_hdl_t rx_handle = getRxHandle(channel);

        SoapySDR_logf(SOAPY_SDR_INFO, "RX channel %zu uses handle %u", channel, rx_handle);

        status = skiq_read_rx_block_size(card, skiq_rx_stream_mode_balanced);
        if (status < 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_block_size failed: "
                                           "card: %u status: %d\n",
                                           card, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_TRACE, "In rx 3");
        rx_block_size_in_bytes = status;
        rx_block_size_in_words = status / 4;

        rx_payload_size_in_bytes = status - SKIQ_RX_HEADER_SIZE_IN_BYTES;
        rx_payload_size_in_words = rx_payload_size_in_bytes / 4;

        SoapySDR_logf(SOAPY_SDR_INFO, "RX payload size in words: %u",
                      rx_payload_size_in_words);

        if (format == "CS16")
        {
            rxUseShort = true;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CS16");
        }
        else if (format == "CF32")
        {
            rxUseShort = false;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CF32");
        }
        else
        {
            throw std::runtime_error("setupStream invalid format '" + format +
                "' -- Only CS16 or CF32 is supported by SoapySidekiq module.");
        }

        // this has to be called after setting sample rate
        int status = 0;
        status = skiq_read_sys_timestamp_freq(this->card, &this->sys_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_sys_timestamp_freq failed: (card %d), status %d",
                           card, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "System Timestamp Freq: %llu", this->sys_freq);

        /* set rx source as iq data */
        if (iq_swap)
        {
            status = skiq_write_iq_order_mode(card, skiq_iq_order_iq);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                             "skiq_write_rx_data_src failed (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
            SoapySDR_logf(SOAPY_SDR_INFO, "RX is set to I then Q order");
        }

        /* set a modest rx timeout to make skiq_receive blocking*/
        status = skiq_set_rx_transfer_timeout(card, 100000);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_set_rx_transfer_timeout failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }

        // Keep the handle locally owned until RX setup is ready to publish it.
        auto stream_handle = std::make_unique<StreamHandle>();
        stream_handle->direction = direction;
        stream_handle->channel = channel;
        stream_handle->rx_handle = rx_handle;
        stream_handle->tx_handle = skiq_tx_hdl_end;
        active_rx_stream = stream_handle.release();
        return reinterpret_cast<SoapySDR::Stream *>(active_rx_stream);
    }
    else if (direction == SOAPY_SDR_TX)
    {
        if (active_tx_stream != nullptr)
        {
            throw std::runtime_error("only one TX stream per device is currently supported");
        }

        //  check the channel configuration
        if (channels.size() > 1)
        {
            throw std::runtime_error("multi-channel TX streams are not currently supported");
        }
        skiq_tx_hdl_t tx_handle = getTxHandle(channel);

        SoapySDR_logf(SOAPY_SDR_INFO, "TX channel %zu uses handle %u", channel, tx_handle);

        if (format == "CS16")
        {
            txUseShort = true;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CS16");
        }
        else if (format == "CF32")
        {
            txUseShort = false;
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CF32");
        }
        else
        {
            throw std::runtime_error(
                "setupStream invalid format '" + format +
                "' -- Only CS16 is supported by SoapySidekiq TX module.");
        }

        status = skiq_read_sys_timestamp_freq(this->card, &this->sys_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_sys_timestamp_freq failed: (card %d), status %d",
                           card, status);
            throw std::runtime_error("");
        }

        // Timestamp setup must succeed before committing TX memory ownership.
        auto stream_handle = std::make_unique<StreamHandle>();
        stream_handle->direction = direction;
        stream_handle->channel = channel;
        stream_handle->rx_handle = skiq_rx_hdl_end;
        stream_handle->tx_handle = tx_handle;
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; ++i)
        {
            p_tx_block[i] = skiq_tx_block_allocate(current_tx_block_size);
            if (p_tx_block[i] == nullptr)
            {
                // Free only the successfully allocated prefix on partial failure.
                for (int allocated = 0; allocated < i; ++allocated)
                {
                    skiq_tx_block_free(p_tx_block[allocated]);
                    p_tx_block[allocated] = nullptr;
                }
                throw std::bad_alloc();
            }
        }
        currTXBuffIndex = 0;

        active_tx_stream = stream_handle.release();
        return reinterpret_cast<SoapySDR::Stream *>(active_tx_stream);
    }
    else
    {
        throw std::runtime_error("invalid direction");
    }
}

void SoapySidekiq::closeStream(SoapySDR::Stream *stream)
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "closeStream");

    StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        throw std::invalid_argument("Cannot close an unknown stream handle");

    if (stream_handle->direction == SOAPY_SDR_RX)
    {
        if (rx_session != nullptr && rx_session->running())
        {
            throw std::runtime_error("deactivate the RX stream before closing it");
        }

        // Preserve retryable hardware ownership if the backend stop fails.
        if (rx_session != nullptr)
        {
            const int status = deactivateStream(stream, 0, 0);
            if (status != 0)
            {
                throw std::runtime_error("Cannot close RX stream after failed deactivation (card " +
                                         std::to_string(card) + ", status " +
                                         std::to_string(status) + ")");
            }
        }

        if (active_rx_stream == stream_handle)
        {
            active_rx_stream = nullptr;
        }
    }
    else if (stream_handle->direction == SOAPY_SDR_TX)
    {
        if (tx_stream_active)
        {
            throw std::runtime_error("deactivate the TX stream before closing it");
        }

        if (active_tx_stream == stream_handle)
        {
            active_tx_stream = nullptr;
        }

        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            skiq_tx_block_free(p_tx_block[i]);
            p_tx_block[i] = nullptr;
        }
    }

    delete stream_handle;
}

size_t SoapySidekiq::getStreamMTU(SoapySDR::Stream *stream) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getStremMTU");

    const StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        throw std::invalid_argument("Cannot query MTU for an unknown stream handle");

    if (stream_handle->direction == SOAPY_SDR_RX)
    {
        return (rx_payload_size_in_words);
    }
    else if (stream_handle->direction == SOAPY_SDR_TX)
    {
        return current_tx_block_size;
    }
    else
    {
        throw std::invalid_argument("Invalid stream direction for MTU");
    }

}

int SoapySidekiq::activateStream(SoapySDR::Stream *stream,
                                 const int flags,
                                 const long long timeNs,
                                 const size_t numElems)
{
    int status = 0;
    StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        return SOAPY_SDR_STREAM_ERROR;

    // Only continuous operation and the legacy next-PPS convention exist.
    // Reject options before any SDK call or asynchronous startup state change.
    if ((flags & ~SOAPY_SDR_HAS_TIME) != 0 || numElems != 0 ||
        ((flags & SOAPY_SDR_HAS_TIME) != 0 && timeNs != 0))
        return SOAPY_SDR_NOT_SUPPORTED;

    SoapySDR_logf(SOAPY_SDR_TRACE, "activateStream");

    if (stream_handle->direction == SOAPY_SDR_RX)
    {
        if (active_rx_stream != stream_handle)
        {
            throw std::runtime_error("RX stream is not the active device RX stream");
        }

        const skiq_rx_hdl_t rx_handle = stream_handle->rx_handle;
        if (rx_session == nullptr)
        {
            return SOAPY_SDR_STREAM_ERROR;
        }

        if ((flags & SOAPY_SDR_HAS_TIME) != 0)
        {
            bool tx_start_pending;
            {
                std::lock_guard<std::mutex> tx_lock(tx_mutex);
                tx_start_pending = _tx_streaming_thread.joinable() && !tx_start_finished;
            }
            if (tx_start_pending)
            {
                /* if skiq_start_tx_streaming_on_1pps is called, then skiq_start_rx_streaming_on_1pps
                 * is called the second one called will block until the first one finishes.
                 * So the second one called will unblock 2 seconds later.
                 * So warn the user */
                SoapySDR_logf(SOAPY_SDR_WARNING, "The skiq_start_tx_streaming_on_1pps is"
                                                 " still blocked waiting for 1pps to occur"
                                                 " so calling activating a RX stream will be"
                                                 " delayed 2 seconds");

            }
        }

        // Coupled clocks may have changed since the last RX setter. Timestamp
        // setup needs only the actual rate, not configured rate or bandwidth.
        uint32_t configured_rate{}, configured_bandwidth{}, actual_bandwidth{};
        double actual_rate = std::numeric_limits<double>::quiet_NaN();
        const int rate_status =
            skiq_read_rx_sample_rate_and_bandwidth(card, rx_handle, &configured_rate, &actual_rate,
                                                   &configured_bandwidth, &actual_bandwidth);
        if (rate_status != 0)
            throw std::runtime_error(
                "RX activation rate read failed (card " + std::to_string(card) + ", handle " +
                std::to_string(rx_handle) + "): " + std::to_string(rate_status));
        const auto sample_rate = soapy_sidekiq::TimestampRate::fromActualRate(actual_rate);
        uint64_t system_frequency = rfTimeSource ? sys_freq : 0;
        if (!rfTimeSource)
        {
            const int read_status = skiq_read_sys_timestamp_freq(card, &system_frequency);
            if (read_status != 0)
                throw std::runtime_error("skiq_read_sys_timestamp_freq failed at RX activation: " +
                                         std::to_string(read_status));
        }
        // The session starts hardware before publishing or creating a worker.
        status = rx_session->activate({static_cast<uint32_t>(rx_handle), rx_payload_size_in_words,
                                       sample_rate, system_frequency, rfTimeSource,
                                       (flags & SOAPY_SDR_HAS_TIME) != 0});
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "starting RX streaming failed (card %u, handle %d), status %d",
                card, rx_handle, status);
            throw std::runtime_error("failed to start RX streaming");
        }

        SoapySDR_logf(SOAPY_SDR_INFO,
                      "started receive streaming on handle: %u",
                      rx_handle);

    }
    else if (stream_handle->direction == SOAPY_SDR_TX)
    {
        if (active_tx_stream != stream_handle)
        {
            throw std::runtime_error("TX stream is not the active device TX stream");
        }

        const skiq_tx_hdl_t tx_handle = stream_handle->tx_handle;

        /* set as iq data */
        if (iq_swap)
        {
            status = skiq_write_iq_order_mode(card, skiq_iq_order_iq);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                             "skiq_write_rx_data_src failed (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
            SoapySDR_logf(SOAPY_SDR_INFO, "TX is set to I then Q order");
        }

        p_tx_block_index = 0;

        //  tx block size
        status =
            skiq_write_tx_block_size(card, tx_handle, current_tx_block_size);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                         "skiq_write_tx_block_size failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "TX block size is: %u", current_tx_block_size);

        //  tx data flow mode
        status = skiq_write_tx_data_flow_mode(card, tx_handle,
                                              skiq_tx_immediate_data_flow_mode);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "skiq_write_tx_data_flow_mode failed (card %u) status %d",
                card, status);
            throw std::runtime_error("");
        }

        // running in aync mode
        status = skiq_write_tx_transfer_mode(card, tx_handle,
                                             skiq_tx_transfer_mode_async);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_tx_transfer_mode failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }

        // configure 4 threads to be safe, too many and it may consume resources
        status = skiq_write_num_tx_threads(card, 4);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_num_tx_threads failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }

        // A new activation must not inherit a failed PPS start's write gate.
        first_transmit = false;

        /* start tx streaming */
        if (flags == SOAPY_SDR_HAS_TIME)
        {
            /* The SDK call blocks until the PPS edge. Keep activateStream
             * nonblocking and let the first write wait for the result. */
            {
                std::lock_guard<std::mutex> lock(tx_mutex);
                tx_start_finished = false;
                tx_start_status = 0;
            }
            _tx_streaming_thread =
                std::thread(&SoapySidekiq::tx_streaming_start, this, tx_handle);

            first_transmit = true;
        }
        else
        {
            status = skiq_start_tx_streaming(card, tx_handle);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_start_tx_streaming failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
            {
                std::lock_guard<std::timed_mutex> lock(tx_status_mutex);
                tx_underruns = 0;
            }
            SoapySDR_logf(SOAPY_SDR_INFO,
                    "started transmit streaming on handle: %u",
                    tx_handle);
        }

        tx_stream_active = true;
        {
            std::scoped_lock lock(tx_mutex, tx_buf_mutex);
            tx_writes_stopping = false;
        }
    }


    return 0;
}

int SoapySidekiq::deactivateStream(SoapySDR::Stream *stream, const int flags,
                                   const long long timeNs)
{
    int status = 0;
    StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        return SOAPY_SDR_STREAM_ERROR;
    if ((flags & ~SOAPY_SDR_HAS_TIME) != 0 || ((flags & SOAPY_SDR_HAS_TIME) != 0 && timeNs != 0))
        return SOAPY_SDR_NOT_SUPPORTED;
    SoapySDR_logf(SOAPY_SDR_TRACE, "deactivateStream");

    if (stream_handle->direction == SOAPY_SDR_RX)
    {
        if (active_rx_stream != stream_handle)
        {
            return SOAPY_SDR_STREAM_ERROR;
        }

        const skiq_rx_hdl_t rx_handle = stream_handle->rx_handle;
        // RxStreamSession joins the worker even when the backend stop fails.
        status = rx_session == nullptr
            ? SOAPY_SDR_STREAM_ERROR
            : rx_session->deactivate((flags & SOAPY_SDR_HAS_TIME) != 0);
        if (status == -ENODEV)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                "RX handle was not streaming while deactivating (card %u, handle %d)",
                card, rx_handle);
        }
        else if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "stopping RX streaming failed (card %u, handle %d), status %d",
                card, rx_handle, status);
            return SOAPY_SDR_STREAM_ERROR;
        }
    }
    else if (stream_handle->direction == SOAPY_SDR_TX)
    {
        if (active_tx_stream != stream_handle)
        {
            return SOAPY_SDR_STREAM_ERROR;
        }

        const skiq_tx_hdl_t tx_handle = stream_handle->tx_handle;
        // Wake blocked writers and wait for their reservations/contexts to
        // unwind before stopping hardware or allowing stream storage to close.
        stopTxWrites();
        // A timed start may still be finishing when the caller stops the
        // stream. Join it before stopping and before destroying the device.
        if (_tx_streaming_thread.joinable())
        {
            _tx_streaming_thread.join();
        }
        if (flags == SOAPY_SDR_HAS_TIME)
        {
            /* stop tx streaming */
            status = skiq_stop_tx_streaming_on_1pps(card, tx_handle, 0);
            if (status != 0)
            {
                if (status == -ENODEV)
                {
                    SoapySDR_logf(SOAPY_SDR_WARNING,
                        "skiq_stop_tx_streaming_on_1pps: handle not streaming (card %u, handle %d), ignoring",
                        card, tx_handle);
                }
                else
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                        "skiq_stop_tx_streaming_on_1pps failed (card %u), status %d",
                        card, status);
                    return SOAPY_SDR_STREAM_ERROR;
                }
            }

        }
        else
        {
            /* stop tx streaming */
            status = skiq_stop_tx_streaming(card, tx_handle);
            if (status != 0)
            {
                if (status == -ENODEV)
                {
                    SoapySDR_logf(SOAPY_SDR_WARNING,
                        "skiq_stop_tx_streaming: handle not streaming (card %u, handle %d), ignoring",
                        card, tx_handle);
                }
                else
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                        "skiq_stop_tx_streaming failed (card %u), status %d",
                        card, status);
                    return SOAPY_SDR_STREAM_ERROR;
                }
            }
        }

        tx_stream_active = false;
    }

    return 0;
}

int SoapySidekiq::readStream(SoapySDR::Stream *stream,
                             void *const *buffs,
                             const size_t numElems,
                             int &flags,
                             long long &timeNs,
                             const long timeoutUs)
{
    const StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        return SOAPY_SDR_STREAM_ERROR;
    if (stream_handle->direction != SOAPY_SDR_RX) return SOAPY_SDR_NOT_SUPPORTED;
    if (active_rx_stream != stream_handle) return SOAPY_SDR_STREAM_ERROR;
    if (rx_session == nullptr) return SOAPY_SDR_STREAM_ERROR;
    flags = 0;
    timeNs = 0;
    if (numElems > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return SOAPY_SDR_OVERFLOW;
    }
    if (timeoutUs < 0 ||
        (numElems != 0 && (buffs == nullptr || buffs[0] == nullptr)))
    {
        return SOAPY_SDR_STREAM_ERROR;
    }

    // The queue stores native CS16; CF32 requests use a temporary conversion buffer.
    std::vector<int16_t> converted_samples;
    int16_t *queue_output = nullptr;
    if (numElems != 0)
    {
        if (rxUseShort)
        {
            queue_output = static_cast<int16_t *>(buffs[0]);
        }
        else
        {
            converted_samples.resize(numElems * 2);
            queue_output = converted_samples.data();
        }
    }

    // Session read honors the requested timeout and translates worker lifecycle
    // transitions into stable queue statuses for the SoapySDR API.
    const auto result = rx_session->read(
        queue_output,
        numElems,
        std::chrono::microseconds(timeoutUs));
    if (result.status == soapy_sidekiq::RxReadStatus::timeout)
    {
        return SOAPY_SDR_TIMEOUT;
    }
    if (result.status == soapy_sidekiq::RxReadStatus::overflow)
        return SOAPY_SDR_OVERFLOW;
    if (result.status != soapy_sidekiq::RxReadStatus::samples)
    {
        return SOAPY_SDR_STREAM_ERROR;
    }

    if (!rxUseShort && result.samples != 0)
    {
        float *output = static_cast<float *>(buffs[0]);
        for (size_t index = 0; index < result.samples * 2; ++index)
        {
            output[index] = static_cast<float>(converted_samples[index]) /
                static_cast<float>(maxValue);
        }
    }
    if (result.has_time)
    {
        flags |= SOAPY_SDR_HAS_TIME;
        timeNs = result.time_ns;
    }
    return static_cast<int>(result.samples);
}

int SoapySidekiq::writeStream(SoapySDR::Stream * stream,
                              const void *const *buffs, const size_t numElems,
                              int &flags, const long long timeNs,
                              const long timeoutUs)
{
    int      status = 0;
    const StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        return SOAPY_SDR_STREAM_ERROR;

    if (stream_handle->direction != SOAPY_SDR_TX)
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    if (active_tx_stream != stream_handle)
    {
        return SOAPY_SDR_STREAM_ERROR;
    }

    // TX is continuous/immediate. Timed writes and burst/fragment flags
    // cannot be honored by the current SDK data-flow configuration.
    if (flags != 0)
        return SOAPY_SDR_NOT_SUPPORTED;

    if (timeoutUs < 0)
    {
        return SOAPY_SDR_STREAM_ERROR;
    }

    {
        std::lock_guard<std::mutex> lock(tx_buf_mutex);
        if (tx_writes_stopping)
            return SOAPY_SDR_STREAM_ERROR;
        ++active_tx_writers;
    }
    soapy_sidekiq::ScopeExit writer(
        [this]() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(tx_buf_mutex);
                --active_tx_writers;
            }
            tx_buf_cv.notify_all();
        });

    // TX has no partial-block staging. Reject malformed requests before PPS
    // waits, pointer access, reservations, or submission. The API return type
    // must be able to represent every accepted sample count.
    if (current_tx_block_size == 0 ||
        numElems > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        numElems % current_tx_block_size != 0)
    {
        return SOAPY_SDR_STREAM_ERROR;
    }
    if (numElems == 0)
        return 0;
    if (buffs == nullptr || buffs[0] == nullptr)
        return SOAPY_SDR_STREAM_ERROR;

    // Clamp the deadline so even LONG_MAX microseconds cannot overflow
    // the clock's (typically nanosecond) representation.
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto budget = std::chrono::microseconds(timeoutUs);
    const auto remaining =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::time_point::max() - now);
    const auto deadline = budget >= remaining ? Clock::time_point::max() : now + budget;

    if (first_transmit)
    {
        SoapySDR_logf(SOAPY_SDR_DEBUG, "writeStream waiting for PPS TX start");
        std::unique_lock<std::mutex> lock(tx_mutex);
        if (!tx_cv.wait_until(lock, deadline,
                              [this] { return tx_start_finished || tx_writes_stopping; }))
        {
            // The asynchronous PPS start is still pending. A later write may
            // wait again; no packet or callback context has been submitted.
            return SOAPY_SDR_TIMEOUT;
        }
        if (tx_writes_stopping || tx_start_status != 0)
        {
            // Keep the gate set so retries cannot transmit after a failed start.
            return SOAPY_SDR_STREAM_ERROR;
        }
        first_transmit = false;
    }

    // Pointer to the location in the input buffer to transmit from
    const char *inbuff_ptr = static_cast<const char *>(buffs[0]);

    uint32_t num_blocks = numElems / current_tx_block_size;

    uint32_t curr_block = 0;

    // total number of bytes that need to be transmitted in this call
    uint32_t tx_block_bytes = current_tx_block_size * 4;
    // Hardware payloads are CS16, but the caller's CF32 buffer uses wider samples.
    const size_t input_block_bytes = current_tx_block_size * 2 *
        (txUseShort ? sizeof(int16_t) : sizeof(float));

    while (curr_block < num_blocks)
    {
        uint64_t observed_completions = 0;
        {
            // The predicate and completion updates share a mutex, so completion
            // cannot disappear between testing the slot and sleeping.
            std::unique_lock<std::mutex> lock(tx_buf_mutex);
            if (!tx_buf_cv.wait_until(
                    lock, deadline,
                    [this] { return tx_writes_stopping || p_tx_status[currTXBuffIndex] == 0; }))
            {
                return curr_block == 0 ? SOAPY_SDR_TIMEOUT
                                       : static_cast<int>(curr_block * current_tx_block_size);
            }
            if (tx_writes_stopping)
                return curr_block == 0 ? SOAPY_SDR_STREAM_ERROR
                                       : static_cast<int>(curr_block * current_tx_block_size);
            p_tx_status[currTXBuffIndex] = 1;
            observed_completions = complete_count;
        }

        {
            // Until the SDK accepts this block, unwind its reservation on any
            // allocation, conversion, or submission failure.
            const auto block_index = currTXBuffIndex;
            soapy_sidekiq::ScopeExit reservation(
                [this, block_index]() noexcept
                {
                    std::lock_guard<std::mutex> lock(tx_buf_mutex);
                    p_tx_status[block_index] = 0;
                });
            auto context = std::make_unique<passedStruct>();
            context->classAddr = this;
            context->txIndex = block_index;

            // Pointer to the location in the output buffer to copy to.
            char *outbuff_ptr = reinterpret_cast<char *>(p_tx_block[currTXBuffIndex]->data);

            // determine if we received short or float
            if (txUseShort)
            {
                // CS16
                memcpy(outbuff_ptr, inbuff_ptr, tx_block_bytes);
            }
            else
            {
                // Clip and normalize exceptional CF32 values before integer storage.
                soapy_sidekiq::convertCf32ToCs16(reinterpret_cast<const float *>(inbuff_ptr),
                                                 reinterpret_cast<std::int16_t *>(outbuff_ptr),
                                                 current_tx_block_size,
                                                 static_cast<float>(this->txMaxValue));
            }

            status = skiq_transmit(this->card, stream_handle->tx_handle,
                                   this->p_tx_block[block_index], context.get());
            if (status == 0)
            {
                // The SDK completion callback now owns context. It may already
                // have run synchronously; do not inspect context after submit.
                context.release();
                reservation.release();
            }
        }
        if (status == SKIQ_TX_ASYNC_SEND_QUEUE_FULL)
        {
            // A completion during skiq_transmit must satisfy this predicate
            // immediately; wait only when no completion has made progress.
            std::unique_lock<std::mutex> lock(tx_buf_mutex);
            const bool progressed =
                tx_writes_stopping ||
                (Clock::now() < deadline &&
                 tx_buf_cv.wait_until(lock, deadline,
                                      [this, observed_completions] {
                                          return tx_writes_stopping ||
                                                 complete_count != observed_completions;
                                      }));
            if (tx_writes_stopping)
                return curr_block == 0 ? SOAPY_SDR_STREAM_ERROR
                                       : static_cast<int>(curr_block * current_tx_block_size);
            if (!progressed)
            {
                return curr_block == 0 ? SOAPY_SDR_TIMEOUT
                                       : static_cast<int>(curr_block * current_tx_block_size);
            }
        }
        else if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_transmit failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }
        else
        {
            curr_block++;

            // move the index into the transmit block array
            currTXBuffIndex = (currTXBuffIndex + 1) % DEFAULT_NUM_BUFFERS;

            // move the pointer to the next block in the writeStream buffer
            inbuff_ptr += input_block_bytes;

        }

    }

    return static_cast<int>(numElems);
}

int SoapySidekiq::readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags,
                                   long long &timeNs, const long timeoutUs)
{
    const StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        return SOAPY_SDR_STREAM_ERROR;
    if (stream_handle->direction != SOAPY_SDR_TX)
        return SOAPY_SDR_NOT_SUPPORTED;
    if (active_tx_stream != stream_handle)
        return SOAPY_SDR_STREAM_ERROR;
    chanMask = 0;
    flags = 0;
    timeNs = 0;
    if (timeoutUs < 0)
        return SOAPY_SDR_STREAM_ERROR;

    {
        std::lock_guard<std::mutex> lock(tx_buf_mutex);
        if (tx_writes_stopping)
            return SOAPY_SDR_STREAM_ERROR;
        ++active_tx_status_readers;
    }
    soapy_sidekiq::ScopeExit reader(
        [this]() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(tx_buf_mutex);
                --active_tx_status_readers;
            }
            tx_buf_cv.notify_all();
        });

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto budget = std::chrono::microseconds(timeoutUs);
    const auto maximum =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::time_point::max() - start);
    const auto deadline = budget >= maximum ? Clock::time_point::max() : start + budget;
    for (;;)
    {
        {
            // One poller consumes each counter increase. Completion callbacks
            // do not acquire this mutex; never hold their TX locks across SDK IO.
            std::unique_lock<std::timed_mutex> status_lock(tx_status_mutex, std::defer_lock);
            if (!status_lock.try_lock_until(deadline))
                return SOAPY_SDR_TIMEOUT;
            bool pending = false;
            {
                std::scoped_lock lock(tx_mutex, tx_buf_mutex);
                if (tx_writes_stopping)
                    return SOAPY_SDR_STREAM_ERROR;
                pending = first_transmit && !tx_start_finished;
                if (first_transmit && tx_start_finished && tx_start_status != 0)
                    return SOAPY_SDR_STREAM_ERROR;
            }
            if (!pending)
            {
                uint32_t errors = 0;
                const int status =
                    skiq_read_tx_num_underruns(card, stream_handle->tx_handle, &errors);
                if (status != 0)
                {
                    status_lock.unlock();
                    SoapySDR_logf(
                        SOAPY_SDR_ERROR,
                        "skiq_read_tx_num_underruns failed (card %u, handle %u), status %d", card,
                        stream_handle->tx_handle, status);
                    return SOAPY_SDR_STREAM_ERROR;
                }
                const bool increased = errors > tx_underruns;
                // Rebase after resets/decreases so future increments remain visible.
                tx_underruns = errors;
                if (increased)
                {
                    chanMask = 1; // Only stream channel zero exists, regardless of SDK handle.
                    return SOAPY_SDR_UNDERFLOW;
                }
            }
        }
        const auto now = Clock::now();
        if (now >= deadline)
            return SOAPY_SDR_TIMEOUT;
        // The SDK exposes a counter, not an event wait. Bound polling and let
        // stopTxWrites wake this call promptly without consuming the timeout.
        std::unique_lock<std::mutex> lock(tx_buf_mutex);
        const auto wake = std::min(deadline, now + std::chrono::milliseconds(1));
        if (tx_buf_cv.wait_until(lock, wake, [this] { return tx_writes_stopping; }))
            return SOAPY_SDR_STREAM_ERROR;
    }
}
