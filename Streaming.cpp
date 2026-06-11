#include <cstring>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <algorithm>

#include "SoapySidekiq.hpp"
#include <SoapySDR/Formats.hpp>
#include <sidekiq_types.h>

SoapySidekiq *SoapySidekiq::thisClassAddr = nullptr;

namespace
{
double fullScaleFromResolution(const uint8_t resolution)
{
    if (resolution == 0 || resolution > 31)
    {
        return 32767.0;
    }

    return static_cast<double>((1u << (resolution - 1)) - 1u);
}
}

double SoapySidekiq::rxFullScaleForHandle(const skiq_rx_hdl_t handle) const
{
    if (handle >= skiq_rx_hdl_end)
    {
        return 32767.0;
    }

    return fullScaleFromResolution(param.rx_param[handle].iq_resolution);
}

double SoapySidekiq::txFullScaleForHandle(const skiq_tx_hdl_t handle) const
{
    if (handle >= skiq_tx_hdl_end)
    {
        return 32767.0;
    }

    return fullScaleFromResolution(param.tx_param[handle].iq_resolution);
}

double SoapySidekiq::nativeFullScale(const int direction, const size_t channel) const
{
    if (direction == SOAPY_SDR_RX)
    {
        return rxFullScaleForHandle(rxHandleForChannel(channel));
    }

    if (direction == SOAPY_SDR_TX)
    {
        return txFullScaleForHandle(txHandleForChannel(channel));
    }

    return 32767.0;
}

long long SoapySidekiq::convert_timestamp_to_nanos(
        const uint64_t timestamp, const uint64_t timestamp_freq) const
{
    const double nanos_per_tic = 1.0/timestamp_freq*1e9;
    const uint64_t whole_nanos_per_tic = static_cast<uint64_t>(nanos_per_tic);
    const double frac_nanos_per_tic = nanos_per_tic - whole_nanos_per_tic;
    const long long nanos =
        static_cast<long long>(timestamp * whole_nanos_per_tic) +
        static_cast<long long>(timestamp * frac_nanos_per_tic);
    return nanos;
}


std::vector<std::string> SoapySidekiq::getStreamFormats(
    const int direction, const size_t channel) const
{
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
    SoapySDR_logf(SOAPY_SDR_TRACE, "getNativeStreamFormat");

    fullScale = nativeFullScale(direction, channel);

    return "CS16";
}

SoapySDR::ArgInfoList SoapySidekiq::getStreamArgsInfo(
            const int direction, const size_t channel) const
{
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


void SoapySidekiq::tx_streaming_start(void)
{
    int status;

    std::unique_lock<std::mutex> lock(tx_mutex);

    SoapySDR_log(SOAPY_SDR_TRACE, "entering tx_streaming_start");

    // wait till called to start running
    _cv.wait(lock, [this] { return tx_start_signal; });

    status = skiq_start_tx_streaming_on_1pps(card, tx_hdl, 0);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_start_tx_streaming_on_1pps failed, (card %u) status %d",
                card, status);
        throw std::runtime_error("");
    }

    SoapySDR_logf(SOAPY_SDR_INFO, "TX start streaming on 1pps completed");

    tx_start_signal = false;
}
/*******************************************************************
 * Sidekiq receive thread
 ******************************************************************/

void SoapySidekiq::rx_receive_operation(void)
{
    try
    {
        rx_receive_operation_impl();
    }
    catch (const std::exception&)
    {
        SoapySDR_log(SOAPY_SDR_WARNING, "Exiting RX Sidekiq Thread due to error");
        rx_receive_operation_exited_due_to_error = true;
    }

}
void SoapySidekiq::rx_receive_operation_impl(void)
{
    int status = 0;

    skiq_rx_block_t *tmp_p_rx_block;
    uint32_t len;
    skiq_rx_hdl_t rcvd_hdl;

    std::unique_lock<std::mutex> lock(rx_mutex);

    // wait till called to start running
    _cv.wait(lock, [this] { return rx_start_signal; });

    //  loop until stream is deactivated
    while (rx_running)
    {
        status = skiq_receive(card, &rcvd_hdl, &tmp_p_rx_block, &len);
        if (status == skiq_rx_status_success)
        {
            if (rcvd_hdl < skiq_rx_hdl_end && rx_handle_enabled[rcvd_hdl])
            {
                if (len != rx_block_size_in_bytes)
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR, 
                                  "received length %d is not the correct block size %d\n",
                                  len, rx_block_size_in_bytes);
                    throw std::runtime_error("");
                }

                // --- Overrun detection: if buffer full, drop half ---
                uint32_t nextWrite = (rxWriteIndex[rcvd_hdl] + 1) % DEFAULT_NUM_BUFFERS;
                if (nextWrite == rxReadIndex[rcvd_hdl])
                {
                    SoapySDR_logf(SOAPY_SDR_WARNING,
                                  "RX ring buffer overrun on handle %u: client too slow, dropping half buffer",
                                  rcvd_hdl);
                    rxReadIndex[rcvd_hdl] =
                        (rxReadIndex[rcvd_hdl] + (DEFAULT_NUM_BUFFERS / 2)) %
                        DEFAULT_NUM_BUFFERS;
                }

                uint64_t this_timestamp = tmp_p_rx_block->rf_timestamp;
                if (!rx_first_block[rcvd_hdl])
                {
                    if (this_timestamp != rx_expected_timestamp[rcvd_hdl])
                    {
                        SoapySDR_log(SOAPY_SDR_WARNING,
                                     "Detected timestamp overflow/missed samples" 
                                     " in RX Sidekiq Thread");
                        SoapySDR_logf(SOAPY_SDR_DEBUG, "expected timestamp %lu, actual %lu",
                                      rx_expected_timestamp[rcvd_hdl], this_timestamp);
                        rx_first_block[rcvd_hdl] = true;
                    }
                }
                if (rx_first_block[rcvd_hdl])
                {
                    rx_first_block[rcvd_hdl] = false;
                }
                rx_expected_timestamp[rcvd_hdl] =
                    this_timestamp + rx_payload_size_in_words;

                // Copy into RAM ring buffer
                memcpy(p_rx_block[rcvd_hdl][rxWriteIndex[rcvd_hdl]],
                       (void *)tmp_p_rx_block,
                       len);
                rxWriteIndex[rcvd_hdl] =
                    (rxWriteIndex[rcvd_hdl] + 1) % DEFAULT_NUM_BUFFERS;
            }
        }
        else if (status == skiq_rx_status_error_overrun)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING, "overrun detected, (card %u)", card);
        }
        else
        {
            if (status != skiq_rx_status_no_data)
            {
                SoapySDR_logf(SOAPY_SDR_FATAL,
                              "skiq_receive failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
        }
    }
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

    if (direction == SOAPY_SDR_RX)
    {
        rx_stream_handles.clear();
        if (channels.empty())
        {
            rx_stream_handles.push_back(rxHandleForChannel(DEFAULT_CHANNEL));
        }
        else
        {
            for (const size_t channel : channels)
            {
                const skiq_rx_hdl_t handle = rxHandleForChannel(channel);
                if (std::find(rx_stream_handles.begin(),
                              rx_stream_handles.end(),
                              handle) != rx_stream_handles.end())
                {
                    throw std::runtime_error("RX channel " + std::to_string(channel) +
                                             " maps to a duplicate Sidekiq handle");
                }

                if (!rx_stream_handles.empty())
                {
                    skiq_rx_hdl_t hdl_conflicts[skiq_rx_hdl_end] = {};
                    uint8_t num_conflicts = 0;
                    status = skiq_read_rx_stream_handle_conflict(
                        card,
                        handle,
                        hdl_conflicts,
                        &num_conflicts);
                    if (status != 0)
                    {
                        SoapySDR_logf(SOAPY_SDR_ERROR,
                                      "skiq_read_rx_stream_handle_conflict failed, card %u, handle %u, status %d",
                                      card,
                                      handle,
                                      status);
                        throw std::runtime_error("");
                    }

                    for (uint8_t conflict = 0; conflict < num_conflicts; conflict++)
                    {
                        if (std::find(rx_stream_handles.begin(),
                                      rx_stream_handles.end(),
                                      hdl_conflicts[conflict]) != rx_stream_handles.end())
                        {
                            throw std::runtime_error("RX channel " + std::to_string(channel) +
                                                     " conflicts with an already selected RX handle");
                        }
                    }
                }

                rx_stream_handles.push_back(handle);
            }
        }

        rx_hdl = rx_stream_handles.front();

        SoapySDR_logf(SOAPY_SDR_INFO, "RX handle: %u", rx_hdl);

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

        for (int h = 0; h < skiq_rx_hdl_end; h++)
        {
            rx_handle_enabled[h] = false;
            rx_first_block[h] = true;
            rx_expected_timestamp[h] = 0;
            rxWriteIndex[h] = 0;
            rxReadIndex[h]  = 0;
            rx_fifo_buffer[h].clear();
            rx_fifo_offset[h] = 0;
            rx_fifo_time_ns[h] = 0;
        }

        // allocate the RAM buffers for each selected RX handle
        for (const auto handle : rx_stream_handles)
        {
            const int h = static_cast<int>(handle);
            rx_handle_enabled[h] = true;

            for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
            {
                if (p_rx_block[h][i] == NULL)
                {
                    p_rx_block[h][i] = (skiq_rx_block_t *)malloc(rx_block_size_in_bytes);
                    if (p_rx_block[h][i] == NULL)
                    {
                        SoapySDR_log(SOAPY_SDR_ERROR, "malloc failed to allocate RX memory");
                        throw std::runtime_error("");
                    }
                }

                memset(p_rx_block[h][i], 0, rx_block_size_in_bytes);
            }
        }

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

        const bool needs_dual_chan_mode =
            std::find(rx_stream_handles.begin(), rx_stream_handles.end(),
                      skiq_rx_hdl_A2) != rx_stream_handles.end() ||
            std::find(rx_stream_handles.begin(), rx_stream_handles.end(),
                      skiq_rx_hdl_B2) != rx_stream_handles.end();
        const skiq_chan_mode_t chan_mode = needs_dual_chan_mode
                ? skiq_chan_mode_dual
                : skiq_chan_mode_single;
        status = skiq_write_chan_mode(card, chan_mode);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_chan_mode failed, card %u, mode %u, status %d",
                          card, chan_mode, status);
            throw std::runtime_error("");
        }

        const uint32_t stream_sample_rate =
            rx_sample_rate == 0 ? DEFAULT_SAMPLE_RATE : rx_sample_rate;
        const uint32_t stream_bandwidth =
            rx_bandwidth == 0 ? DEFAULT_BANDWIDTH : rx_bandwidth;
        writeRxSampleRateAndBandwidth(
                rx_stream_handles, stream_sample_rate, stream_bandwidth);

        for (const auto handle : rx_stream_handles)
        {
            if (rx_center_frequency_by_handle[handle] == 0)
            {
                rx_center_frequency_by_handle[handle] =
                    rx_center_frequency == 0 ? DEFAULT_FREQUENCY
                                             : rx_center_frequency;
            }
            writeRxFrequency(handle, rx_center_frequency_by_handle[handle]);
        }

        // this has to be called after setting sample rate
        status = skiq_read_sys_timestamp_freq(this->card, &this->sys_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_sys_timestamp_freq failed: (card %d), status %d",
                           card, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "System Timestamp Freq: %llu", this->sys_freq);
        rx_fifo_time_step_ns = static_cast<long long>(NANOS_IN_SEC / rx_sample_rate);

        /* set rx sample order */
        if (iq_swap == true)
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

        const skiq_data_src_t rx_data_source =
            counter ? skiq_data_src_counter : skiq_data_src_iq;
        for (const auto handle : rx_stream_handles)
        {
            status = skiq_write_rx_data_src(card, handle, rx_data_source);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_write_rx_data_src failed (card %u, handle %u) status %d",
                              card, handle, status);
                throw std::runtime_error("");
            }
        }
        SoapySDR_logf(SOAPY_SDR_INFO,
                      "RX data source set to %s for %zu handle(s)",
                      counter ? "counter" : "IQ",
                      rx_stream_handles.size());

        /* set a modest rx timeout to make skiq_receive blocking*/
        status = skiq_set_rx_transfer_timeout(card, 100000);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_set_rx_transfer_timeout failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }

        return RX_STREAM;
    }
    else if (direction == SOAPY_SDR_TX)
    {
        //  check the channel configuration
        if (channels.size() > 1)
        {
            throw std::runtime_error("only one TX channel is supported simultaneously");
        }

        const size_t tx_soapy_channel =
            channels.empty() ? DEFAULT_CHANNEL : channels.at(0);
        tx_hdl = txHandleForChannel(tx_soapy_channel);

        SoapySDR_logf(SOAPY_SDR_INFO, "The TX handle is: %u", tx_hdl);

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
                "' -- Only CS16 or CF32 is supported by SoapySidekiq TX module.");
        }

        tx_bytes_per_sample = txUseShort ? sizeof(int16_t) * 2 : sizeof(float) * 2;
        tx_staging_buffer.resize(tx_bytes_per_sample * current_tx_block_size);
        tx_staging_fill = 0;

        // Allocate buffers
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            p_tx_block[i] = skiq_tx_block_allocate(current_tx_block_size);
        }
        currTXBuffIndex = 0;

        // Program the selected TX handle with cached/default parameters.
        setSampleRate(SOAPY_SDR_TX, tx_soapy_channel,
                      tx_sample_rate == 0 ? DEFAULT_SAMPLE_RATE : tx_sample_rate);
        setBandwidth(SOAPY_SDR_TX, tx_soapy_channel,
                     tx_bandwidth == 0 ? DEFAULT_BANDWIDTH : tx_bandwidth);
        setFrequency(SOAPY_SDR_TX, tx_soapy_channel,
                     tx_center_frequency == 0 ? DEFAULT_FREQUENCY
                                              : tx_center_frequency);

        status = skiq_read_sys_timestamp_freq(this->card, &this->sys_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_sys_timestamp_freq failed: (card %d), status %d",
                           card, status);
            throw std::runtime_error("");
        }
        
        return TX_STREAM;
    }
    else
    {
        throw std::runtime_error("invalid direction");
    }
}

void SoapySidekiq::closeStream(SoapySDR::Stream *stream)
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "closeStream");

    if (stream == RX_STREAM)
    {
        for (int h = 0; h < skiq_rx_hdl_end; h++)
        {
            for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
            {
                free(p_rx_block[h][i]);
                p_rx_block[h][i] = NULL;
            }
            rx_fifo_buffer[h].clear();
            rx_fifo_offset[h] = 0;
            rx_handle_enabled[h] = false;
        }
        rx_stream_handles.clear();
    }
    else if (stream == TX_STREAM)
    {
        for (int i = 0; i < DEFAULT_NUM_BUFFERS; i++)
        {
            skiq_tx_block_free(p_tx_block[i]);
            p_tx_block[i] = NULL;
        }
        tx_staging_buffer.clear();
        tx_staging_fill = 0;
        tx_bytes_per_sample = 0;
    }
}

size_t SoapySidekiq::getStreamMTU(SoapySDR::Stream *stream) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getStremMTU");

    if (stream == RX_STREAM)
    {
        return (rx_payload_size_in_words);
    }
    else if (stream == TX_STREAM)
    {
        return current_tx_block_size;
    }
    else
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    return 0;
}

int SoapySidekiq::activateStream(SoapySDR::Stream *stream,
                                 const int flags,
                                 const long long timeNs,
                                 const size_t numElems)
{
    int status = 0;
    bool rx_streaming_on_1pps_started = false;

    SoapySDR_logf(SOAPY_SDR_TRACE, "activateStream");

    if (stream == RX_STREAM)
    {
        for (const auto handle : rx_stream_handles)
        {
            rxWriteIndex[handle] = 0;
            rxReadIndex[handle]  = 0;
            rx_first_block[handle] = true;
            rx_expected_timestamp[handle] = 0;
            rx_fifo_buffer[handle].clear();
            rx_fifo_offset[handle] = 0;
        }

        //  start the receive thread
        if (!_rx_receive_thread.joinable())
        {
            rx_running = true;
            rx_start_signal = false;

            _rx_receive_thread =
                std::thread(&SoapySidekiq::rx_receive_operation, this);
        }

        std::lock_guard<std::mutex> lock(rx_mutex);


        /* start rx streaming */
        if (flags == SOAPY_SDR_HAS_TIME)
        {
            if (tx_start_signal == true)
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

            rx_streaming_on_1pps_started = true;
            if (rx_stream_handles.size() > 1)
            {
                status = skiq_start_rx_streaming_multi_on_trigger(
                    card,
                    rx_stream_handles.data(),
                    rx_stream_handles.size(),
                    skiq_trigger_src_1pps,
                    0);
            }
            else
            {
                status = skiq_start_rx_streaming_on_1pps(card, rx_hdl, 0);
            }
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_start_rx_streaming_on_1pps failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
            rx_streaming_on_1pps_started = false;
        }
        else
        {
            if (rx_stream_handles.size() > 1)
            {
                status = skiq_start_rx_streaming_multi_immediate(
                    card,
                    rx_stream_handles.data(),
                    rx_stream_handles.size());
            }
            else
            {
                status = skiq_start_rx_streaming(card, rx_hdl);
            }
            if (status !=0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_start_rx_streaming failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
        }

        // Notify the thread to run
        rx_start_signal = true;
        _cv.notify_one();  

        SoapySDR_logf(SOAPY_SDR_INFO,
                      "started receive streaming on %zu handle(s)",
                      rx_stream_handles.size());
                    
    }
    else if (stream == TX_STREAM)
    {
        thisClassAddr = this;

        /* set as iq data */
        if (iq_swap == true)
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
            skiq_write_tx_block_size(card, tx_hdl, current_tx_block_size);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                         "skiq_write_tx_block_size failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "TX block size is: %u", current_tx_block_size);

        //  tx data flow mode
        status = skiq_write_tx_data_flow_mode(card, tx_hdl,
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
        status = skiq_write_tx_transfer_mode(card, tx_hdl,
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

        /* start tx streaming */
        if (flags == SOAPY_SDR_HAS_TIME)
        {
            if (rx_streaming_on_1pps_started == true)
            {
                /* if skiq_start_rx_streaming_on_1pps is called, then skiq_start_tx_streaming_on_1pps 
                 * is called the second one called will block until the first one finishes.  
                 * So the second one called will unblock 2 seconds later. 
                 * So warn the user */
                SoapySDR_logf(SOAPY_SDR_WARNING, "The skiq_start_rx_streaming_on_1pps is" 
                                                 " still blocked waiting for 1pps to occur"
                                                 " so calling activating a TX stream will be"
                                                 " delayed 2 seconds");

            }
            /* skiq_start_rx_streaming_on_1pps blocks until data starts flowing
             * but this function needs to return immediately so the application can start
             * sending in blocks.
             * So this will start a thread to handle the start_streaming call */
            tx_start_signal = false;
            _tx_streaming_thread =
                std::thread(&SoapySidekiq::tx_streaming_start, this);

            first_transmit = true;

            // Notify the thread to run
            tx_start_signal = true;
            _cv.notify_one();  
        }
        else
        {
            status = skiq_start_tx_streaming(card, tx_hdl);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_start_tx_streaming failed, (card %u) status %d",
                              card, status);
                throw std::runtime_error("");
            }
            SoapySDR_logf(SOAPY_SDR_INFO,
                    "started transmit streaming on handle: %u",
                    tx_hdl);
        }
    }


    return 0;
}

int SoapySidekiq::deactivateStream(SoapySDR::Stream *stream, const int flags,
                                   const long long timeNs)
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "deactivateStream");

    if (stream == RX_STREAM)
    {
        // stop receive thread
        rx_running = false;

        /* stop rx streaming */
        if (flags == SOAPY_SDR_HAS_TIME)
        {
            if (rx_stream_handles.size() > 1)
            {
                status = skiq_stop_rx_streaming_multi_immediate(
                    card,
                    rx_stream_handles.data(),
                    rx_stream_handles.size());
            }
            else
            {
                status = skiq_stop_rx_streaming_on_1pps(card, rx_hdl, 0);
            }
            if (status != 0)
            {
                if (status == -ENODEV) // Handle not streaming
                {
                    SoapySDR_logf(SOAPY_SDR_WARNING,
                        "skiq_stop_rx_streaming_on_1pps: handle not streaming" 
                        " (card %u, handle %d), ignoring",
                        card, rx_hdl);
                }
                else
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                        "skiq_stop_rx_streaming_on_1pps failed," 
                        " (card %u) handle %d, status %d",
                        card, rx_hdl, status);
                    throw std::runtime_error("");
                }
            }
        }
        else
        {
            if (rx_stream_handles.size() > 1)
            {
                status = skiq_stop_rx_streaming_multi_immediate(
                    card,
                    rx_stream_handles.data(),
                    rx_stream_handles.size());
            }
            else
            {
                status = skiq_stop_rx_streaming(card, rx_hdl);
            }
            if (status != 0)
            {
                if (status == -ENODEV) // Handle not streaming
                {
                    SoapySDR_logf(SOAPY_SDR_WARNING,
                        "skiq_stop_rx_streaming: handle not streaming (card %u, handle %d), ignoring",
                        card, rx_hdl);
                }
                else
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                        "skiq_stop_rx_streaming failed, (card %u) handle %d, status %d",
                        card, rx_hdl, status);
                    throw std::runtime_error("");
                }
            }
        }

        /* wait till the rx thread is done */
        if (_rx_receive_thread.joinable())
        {
            _rx_receive_thread.join();
        }
    }
    else if (stream == TX_STREAM)
    {
        if (tx_staging_fill > 0 && !tx_staging_buffer.empty())
        {
            memset(tx_staging_buffer.data() + tx_staging_fill,
                   0,
                   tx_staging_buffer.size() - tx_staging_fill);
            status = transmitBlock(tx_staging_buffer.data());
            tx_staging_fill = 0;
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "failed to flush final TX block, status %d",
                              status);
                throw std::runtime_error("");
            }
        }

        if (flags == SOAPY_SDR_HAS_TIME)
        {
            /* stop tx streaming */
            status = skiq_stop_tx_streaming_on_1pps(card, tx_hdl, 0);
            if (status != 0)
            {
                if (status == -ENODEV)
                {
                    SoapySDR_logf(SOAPY_SDR_WARNING,
                        "skiq_stop_tx_streaming_on_1pps: handle not streaming (card %u, handle %d), ignoring",
                        card, tx_hdl);
                }
                else
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                        "skiq_stop_tx_streaming_on_1pps failed (card %u), status %d",
                        card, status);
                    throw std::runtime_error("");
                }
            }

            /* verify the tx thread is done */
            if (_tx_streaming_thread.joinable())
            {
                _tx_streaming_thread.join();
            }
        }
        else
        {
            /* stop tx streaming */
            status = skiq_stop_tx_streaming(card, tx_hdl);
            if (status != 0)
            {
                if (status == -ENODEV)
                {
                    SoapySDR_logf(SOAPY_SDR_WARNING,
                        "skiq_stop_tx_streaming: handle not streaming (card %u, handle %d), ignoring",
                        card, tx_hdl);
                }
                else
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                        "skiq_stop_tx_streaming failed (card %u), status %d",
                        card, status);
                    throw std::runtime_error("");
                }
            }
        }
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
    if (stream != RX_STREAM) return SOAPY_SDR_NOT_SUPPORTED;
    if (rx_receive_operation_exited_due_to_error) return SOAPY_SDR_STREAM_ERROR;
    if (rx_stream_handles.empty()) return SOAPY_SDR_STREAM_ERROR;

    size_t samples_done = 0;
    bool timestamp_set = false;
    long waitTime = (timeoutUs == 0) ? SLEEP_1SEC : timeoutUs;

    auto block_time_ns = [this](const skiq_rx_block_t *block_ptr) -> long long
    {
        if (this->rfTimeSource)
            return convert_timestamp_to_nanos(block_ptr->rf_timestamp, rx_sample_rate);
        return convert_timestamp_to_nanos(block_ptr->sys_timestamp, sys_freq);
    };

    auto available_samples = [this](const skiq_rx_hdl_t handle) -> size_t
    {
        const size_t fifo_left =
            (rx_fifo_buffer[handle].size() / 2) - rx_fifo_offset[handle];
        if (fifo_left > 0)
        {
            return fifo_left;
        }

        if (rxReadIndex[handle] != rxWriteIndex[handle])
        {
            return rx_payload_size_in_words;
        }

        return 0;
    };

    auto copy_samples = [this, &block_time_ns, &timestamp_set, &timeNs, &flags](
        const skiq_rx_hdl_t handle,
        void *output_buf,
        const size_t output_offset,
        const size_t count,
        const bool timestamps_from_this_handle)
    {
        const size_t fifo_left =
            (rx_fifo_buffer[handle].size() / 2) - rx_fifo_offset[handle];

        if (fifo_left > 0)
        {
            if (timestamps_from_this_handle && !timestamp_set)
            {
                timeNs = rx_fifo_time_ns[handle] +
                         static_cast<long long>(rx_fifo_offset[handle]) *
                             rx_fifo_time_step_ns;
                flags = SOAPY_SDR_HAS_TIME;
                timestamp_set = true;
            }

            if (rxUseShort)
            {
                int16_t *out_ptr = reinterpret_cast<int16_t *>(output_buf);
                memcpy(out_ptr + output_offset * 2,
                       rx_fifo_buffer[handle].data() + rx_fifo_offset[handle] * 2,
                       count * 2 * sizeof(int16_t));
            }
            else
            {
                float *out_ptr = reinterpret_cast<float *>(output_buf);
                const float scale =
                    static_cast<float>(rxFullScaleForHandle(handle));
                for (size_t i = 0; i < count; ++i)
                {
                    out_ptr[(output_offset + i) * 2] =
                        float(rx_fifo_buffer[handle][(rx_fifo_offset[handle] + i) * 2]) /
                        scale;
                    out_ptr[(output_offset + i) * 2 + 1] =
                        float(rx_fifo_buffer[handle][(rx_fifo_offset[handle] + i) * 2 + 1]) /
                        scale;
                }
            }

            rx_fifo_offset[handle] += count;
            if (rx_fifo_offset[handle] * 2 >= rx_fifo_buffer[handle].size())
            {
                rx_fifo_buffer[handle].clear();
                rx_fifo_offset[handle] = 0;
                rx_fifo_time_ns[handle] = 0;
            }
            return;
        }

        skiq_rx_block_t *block_ptr = p_rx_block[handle][rxReadIndex[handle]];
        const volatile int16_t *block_data = block_ptr->data;

        if (timestamps_from_this_handle && !timestamp_set)
        {
            timeNs = block_time_ns(block_ptr);
            flags = SOAPY_SDR_HAS_TIME;
            timestamp_set = true;
        }

        if (rxUseShort)
        {
            int16_t *out_ptr = reinterpret_cast<int16_t *>(output_buf);
            memcpy(out_ptr + output_offset * 2,
                   (const void *)block_data,
                   count * 2 * sizeof(int16_t));
        }
        else
        {
            float *out_ptr = reinterpret_cast<float *>(output_buf);
            const float scale =
                static_cast<float>(rxFullScaleForHandle(handle));
            for (size_t i = 0; i < count; ++i)
            {
                out_ptr[(output_offset + i) * 2] =
                    float(block_data[i * 2]) / scale;
                out_ptr[(output_offset + i) * 2 + 1] =
                    float(block_data[i * 2 + 1]) / scale;
            }
        }

        if (count < rx_payload_size_in_words)
        {
            const size_t leftovers = rx_payload_size_in_words - count;
            rx_fifo_buffer[handle].resize(leftovers * 2);
            for (size_t i = 0; i < leftovers * 2; ++i)
            {
                rx_fifo_buffer[handle][i] = block_data[count * 2 + i];
            }
            rx_fifo_offset[handle] = 0;
            rx_fifo_time_ns[handle] =
                block_time_ns(block_ptr) +
                static_cast<long long>(count) * rx_fifo_time_step_ns;
        }

        rxReadIndex[handle] = (rxReadIndex[handle] + 1) % DEFAULT_NUM_BUFFERS;
    };

    while (samples_done < numElems)
    {
        while (waitTime > 0)
        {
            bool all_channels_ready = true;
            for (const auto handle : rx_stream_handles)
            {
                if (available_samples(handle) == 0)
                {
                    all_channels_ready = false;
                    break;
                }
            }

            if (all_channels_ready)
            {
                break;
            }

            usleep(DEFAULT_SLEEP_US);
            waitTime -= DEFAULT_SLEEP_US;
        }

        if (waitTime <= 0)
        {
            return (samples_done > 0) ? samples_done : SOAPY_SDR_TIMEOUT;
        }

        size_t to_copy = numElems - samples_done;
        for (const auto handle : rx_stream_handles)
        {
            to_copy = std::min(to_copy, available_samples(handle));
        }

        for (size_t chan = 0; chan < rx_stream_handles.size(); chan++)
        {
            copy_samples(rx_stream_handles[chan],
                         buffs[chan],
                         samples_done,
                         to_copy,
                         chan == 0);
        }
        samples_done += to_copy;
    }
    return samples_done;
}

int SoapySidekiq::transmitBlock(const uint8_t *inbuff_ptr)
{
    int status = 0;
    uint8_t *outbuff_ptr = (uint8_t *)p_tx_block[currTXBuffIndex]->data;
    const size_t tx_block_bytes = current_tx_block_size * sizeof(int16_t) * 2;

    if (txUseShort)
    {
        memcpy(outbuff_ptr, inbuff_ptr, tx_block_bytes);
    }
    else
    {
        const float *float_inbuff = reinterpret_cast<const float *>(inbuff_ptr);
        int16_t *short_outbuff = reinterpret_cast<int16_t *>(outbuff_ptr);
        const float scale = static_cast<float>(txFullScaleForHandle(tx_hdl));

        for (size_t i = 0; i < current_tx_block_size * 2; i++)
        {
            float v = float_inbuff[i] * scale;
            v = std::max(-32768.0f, std::min(32767.0f, v));
            short_outbuff[i] = static_cast<int16_t>(v);
        }
    }

    while (true)
    {
        // need to make sure that we don't update the timestamp of a packet
        // that is already in use
        tx_buf_mutex.lock();
        if (p_tx_status[currTXBuffIndex] == 0)
        {
            p_tx_status[currTXBuffIndex] = 1;
        }
        else
        {
            tx_buf_mutex.unlock();
            pthread_mutex_lock(&space_avail_mutex);
            // wait for a packet to complete
            space_avail = false;
            pthread_cond_wait(&space_avail_cond, &space_avail_mutex);
            pthread_mutex_unlock(&space_avail_mutex);

            // space available so try again
            continue;
        }
        tx_buf_mutex.unlock();

        tx_contexts[currTXBuffIndex].classAddr = this;
        tx_contexts[currTXBuffIndex].txIndex = currTXBuffIndex;

        // transmit the buffer
        status = skiq_transmit(this->card,
                               this->tx_hdl,
                               this->p_tx_block[currTXBuffIndex],
                               &tx_contexts[currTXBuffIndex]);
        if (status == SKIQ_TX_ASYNC_SEND_QUEUE_FULL)
        {
            // update the in use status since we didn't actually send it yet
            tx_buf_mutex.lock();
            p_tx_status[currTXBuffIndex] = 0;
            tx_buf_mutex.unlock();

            // if there's no space left to send, wait until there should be space available
            pthread_mutex_lock(&space_avail_mutex);

            // wait for a packet to complete
            while (!space_avail)
            {
                pthread_cond_wait(&space_avail_cond, &space_avail_mutex);
            }
            space_avail = false;
            pthread_mutex_unlock(&space_avail_mutex);
        }
        else if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_transmit failed, (card %u) status %d",
                          card, status);
            throw std::runtime_error("");
        }
        else
        {
            // move the index into the transmit block array
            currTXBuffIndex = (currTXBuffIndex + 1) % DEFAULT_NUM_BUFFERS;
            return 0;
        }
    }
}

int SoapySidekiq::writeStream(SoapySDR::Stream * stream,
                              const void *const *buffs, const size_t numElems,
                              int &flags, const long long timeNs,
                              const long timeoutUs)
{
    int status = 0;

    if (stream != TX_STREAM)
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    if (first_transmit == true)
    {
        SoapySDR_logf(SOAPY_SDR_DEBUG, "writeStream waiting on enabled");

        pthread_mutex_lock(&tx_enabled_mutex);
        pthread_cond_wait(&tx_enabled_cond, &tx_enabled_mutex);
        pthread_mutex_unlock(&tx_enabled_mutex);
        first_transmit = false;
    }

    if (tx_bytes_per_sample == 0)
    {
        tx_bytes_per_sample = txUseShort ? sizeof(int16_t) * 2 : sizeof(float) * 2;
        tx_staging_buffer.resize(tx_bytes_per_sample * current_tx_block_size);
    }

    const uint8_t *input = reinterpret_cast<const uint8_t *>(buffs[0]);
    size_t bytes_left = numElems * tx_bytes_per_sample;
    size_t input_offset = 0;

    while (bytes_left > 0)
    {
        const size_t space_left = tx_staging_buffer.size() - tx_staging_fill;
        const size_t chunk = std::min(space_left, bytes_left);

        memcpy(tx_staging_buffer.data() + tx_staging_fill,
               input + input_offset,
               chunk);

        tx_staging_fill += chunk;
        input_offset += chunk;
        bytes_left -= chunk;

        if (tx_staging_fill == tx_staging_buffer.size())
        {
            status = transmitBlock(tx_staging_buffer.data());
            if (status != 0)
            {
                return SOAPY_SDR_STREAM_ERROR;
            }
            tx_staging_fill = 0;
        }
    }

    return numElems;
}

int SoapySidekiq::readStreamStatus(SoapySDR::Stream *stream,
                                  size_t &chanMask,
                                  int &flags,
                                  long long &timeNs,
                                  const long timeoutUs)
{
    int status = 0;
    uint32_t errors = 0;

    if (stream != TX_STREAM)
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    /* This call will return a cumulative number of underruns since start
     * streaming */
    status = skiq_read_tx_num_underruns(this->card, this->tx_hdl, &errors);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_tx_num_underruns failed, (card %u) status %d",
                      this->card, status);
        throw std::runtime_error("");
    }

    // if the total changed since last call indicate UNDERFLOW
    if (errors > this->tx_underruns)
    {
        SoapySDR_logf(SOAPY_SDR_INFO,
                      "Number of underruns %u",
                      errors);
        this->tx_underruns = errors;
        return SOAPY_SDR_UNDERFLOW;
    }
    else
    {
        return 0;
    }

}
