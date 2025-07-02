#include <cstring>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>

#include "SoapySidekiq.hpp"
#include <SoapySDR/Formats.hpp>
#include <sidekiq_types.h>

SoapySidekiq *SoapySidekiq::thisClassAddr = nullptr;

// Attempted to put this in SoapySidekiq.hpp and it would not link
// could not understand why
bool   rx_start_signal = false;
bool   tx_start_signal = false;

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

    if (direction == SOAPY_SDR_RX)
    {
        formats.push_back(SOAPY_SDR_CF32);
    }

    return formats;
}

std::string SoapySidekiq::getNativeStreamFormat(const int    direction,
                                                const size_t channel,
                                                double &     fullScale) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getNativeStreamFormat");

    fullScale = this->maxValue;

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

void SoapySidekiq::rx_receive_operation(void)
{
    try
    {
        rx_receive_operation_impl();
        SoapySDR_log(SOAPY_SDR_INFO, "Exiting RX Sidekiq Thread");
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
    uint32_t         len;
    bool             first = true;
    uint64_t         expected_timestamp = 0;
    uint64_t         last_timestamp = 0;
    uint64_t         overrun_counter = 0;
    skiq_rx_hdl_t rcvd_hdl;

    std::unique_lock<std::mutex> lock(rx_mutex);

    SoapySDR_log(SOAPY_SDR_TRACE, "entering rx_receive_thread");

    _cv.wait(lock, [this] { return rx_start_signal; });

    SoapySDR_log(SOAPY_SDR_INFO, "Starting RX Sidekiq Thread loop");

    while (rx_running)
    {
        // ---- Overrun detection: If the buffer is full, advance tail to drop half of buffer ----
        int nextWrite = (rxWriteIndex + 1) % DEFAULT_NUM_BUFFERS;
        if (nextWrite == rxReadIndex)
        {
            SoapySDR_log(SOAPY_SDR_WARNING, "RX ring buffer overrun: client too slow, dropping half buffer");
            rxReadIndex = (rxReadIndex + (DEFAULT_NUM_BUFFERS / 2)) % DEFAULT_NUM_BUFFERS;
            overrun_counter++;
        }

        status = skiq_receive(card, &rcvd_hdl, &tmp_p_rx_block, &len);
        if (status == skiq_rx_status_success)
        {
            if (rcvd_hdl == rx_hdl)
            {
                if (len != rx_block_size_in_bytes)
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR, "received length %d is not the correct block size %d\n",
                            len, rx_block_size_in_bytes);
                    throw std::runtime_error("");
                }

                // --- Timestamp integrity check, like gr-sidekiq ---
                uint64_t this_timestamp = tmp_p_rx_block->rf_timestamp;
                if (!first)
                {
                    uint64_t expected_ts = last_timestamp + rx_payload_size_in_words;
                    if (this_timestamp != expected_ts)
                    {
                        SoapySDR_log(SOAPY_SDR_WARNING,
                                "Detected timestamp overflow/missed samples in RX Sidekiq Thread");
                        SoapySDR_logf(SOAPY_SDR_DEBUG, "expected timestamp %lu, actual %lu",
                                expected_ts, this_timestamp);
                        first = true; // restart
                    }
                }
                if (first) {
                    first = false;
                }
                last_timestamp = this_timestamp;

                // Copy into RAM ring buffer
                memcpy(p_rx_block[rxWriteIndex], (void *)tmp_p_rx_block, len);
                rxWriteIndex = (rxWriteIndex + 1) % DEFAULT_NUM_BUFFERS;
            }
        }
        else if (status != skiq_rx_status_no_data)
        {
            SoapySDR_logf(SOAPY_SDR_FATAL,
                    "skiq_receive failed, (card %u) status %d",
                    card, status);
            throw std::runtime_error("");
        }
    }

    SoapySDR_log(SOAPY_SDR_INFO, "Exiting RX Sidekiq Thread");
}

int SoapySidekiq::readStream(SoapySDR::Stream *stream, void *const *buffs,
                             const size_t numElems, int &flags,
                             long long &timeNs, const long timeoutUs)
{
    if (stream != RX_STREAM)
        return SOAPY_SDR_NOT_SUPPORTED;
    else if (rx_receive_operation_exited_due_to_error)
        return SOAPY_SDR_STREAM_ERROR;

    // ---- Enforce that client must read multiples of RX block size (like gr-sidekiq) ----
    if (numElems % rx_payload_size_in_words != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "numElems must be a multiple of the RX block size numElems %zu, block size %u",
                     numElems, rx_payload_size_in_words);
        throw std::runtime_error("");
    }

    long waitTime = timeoutUs;
    if (waitTime == 0)
        waitTime = SLEEP_1SEC;

    // Wait for enough blocks
    size_t blocks_needed = numElems / rx_payload_size_in_words;
    size_t blocks_read = 0;
    char *buff_ptr = (char *)buffs[0];

    while (blocks_read < blocks_needed)
    {
        while ((rxReadIndex == rxWriteIndex) && (waitTime > 0))
        {
            usleep(DEFAULT_SLEEP_US);
            waitTime -= DEFAULT_SLEEP_US;
        }
        if (waitTime <= 0)
        {
            SoapySDR_log(SOAPY_SDR_DEBUG, "readStream timed out");
            return SOAPY_SDR_TIMEOUT;
        }

        skiq_rx_block_t *block_ptr = p_rx_block[rxReadIndex];
        char *ringbuffer_ptr = (char *)((char *)block_ptr->data);

        // Timestamp reporting (use first block's timestamp)
        if (blocks_read == 0)
        {
            if (this->rfTimeSource == true)
                timeNs = convert_timestamp_to_nanos(block_ptr->rf_timestamp, rx_sample_rate);
            else
                timeNs = convert_timestamp_to_nanos(block_ptr->sys_timestamp, sys_freq);
            flags = SOAPY_SDR_HAS_TIME;
        }

        // Copy block
        if (rxUseShort)
        {
            memcpy(buff_ptr, ringbuffer_ptr, rx_payload_size_in_bytes);
        }
        else
        {
            float *dbuff_ptr = (float *)buff_ptr;
            int16_t *source = (int16_t *)ringbuffer_ptr;
            int short_ctr = 0;
            for (size_t i = 0; i < rx_payload_size_in_words; i++)
            {
                *dbuff_ptr++ = (float)source[short_ctr + 1] / this->maxValue;
                *dbuff_ptr++ = (float)source[short_ctr] / this->maxValue;
                short_ctr += 2;
            }
        }

        buff_ptr += rx_payload_size_in_bytes;
        rxReadIndex = (rxReadIndex + 1) % DEFAULT_NUM_BUFFERS;
        blocks_read++;
    }

    // NumElems samples have been written to user's buffer
    return numElems;
}

int SoapySidekiq::writeStream(SoapySDR::Stream * stream,
                              const void *const *buffs, const size_t numElems,
                              int &flags, const long long timeNs,
                              const long timeoutUs)
{
    int      status = 0;

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

    if (numElems % current_tx_block_size != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "numElems must be a multiple of the TX MTU size "
                     " numElems %d, block size %u",
                     numElems, current_tx_block_size);
        throw std::runtime_error("");
    }

    // Pointer to the location in the input buffer to transmit from
    char *inbuff_ptr = (char *)(buffs[0]);

    uint32_t num_blocks = numElems / current_tx_block_size;

    uint32_t curr_block = 0;

    // total number of bytes that need to be transmitted in this call
    uint32_t tx_block_bytes = current_tx_block_size * 4;

    while (curr_block < num_blocks)
    {
        // Pointer to the location in the output buffer to copy to.
        char *outbuff_ptr =
                    (char *)p_tx_block[currTXBuffIndex]->data;

        // determine if we received short or float
        if (txUseShort == true)
        {
            // CS16
            memcpy(outbuff_ptr, inbuff_ptr, tx_block_bytes);
        }
        else
        {
            // float
            float *  float_inbuff = (float *)inbuff_ptr;
            uint32_t words_left = current_tx_block_size;
            uint16_t * new_outbuff = (uint16_t *)outbuff_ptr;

            int short_ctr = 0;
            for (uint32_t i = 0; i < words_left; i++)
            {
                new_outbuff[short_ctr + 1] = (uint16_t)(float_inbuff[short_ctr + 1] *
                                              this->maxValue);

                new_outbuff[short_ctr] = (uint16_t)(float_inbuff[short_ctr] *
                                          this->maxValue);
                short_ctr += 2;
            }
        }


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

        // Create the structure that is passed in p_user
        passedStructInstance = new passedStruct;
        passedStructInstance->classAddr = this;
        passedStructInstance->txIndex = currTXBuffIndex;

        // transmit the buffer
        status = skiq_transmit(this->card,
                               this->tx_hdl,
                               this->p_tx_block[currTXBuffIndex],
                               passedStructInstance);
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
            curr_block++;

            // move the index into the transmit block array
            currTXBuffIndex = (currTXBuffIndex + 1) % DEFAULT_NUM_BUFFERS;

            // move the pointer to the next block in the writeStream buffer
            inbuff_ptr += (current_tx_block_size * 4);
 
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


