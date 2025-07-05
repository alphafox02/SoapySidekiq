#include "SoapySidekiq.hpp"
#include <SoapySDR/Formats.hpp>
#include <cstring>
#include <cinttypes>
#include <iostream>
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
bool                          SoapySidekiq::rx_running;


// compares two strings and if equal range and equal values per character
// returns true.
bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](char a, char b)
        {
            return std::tolower(a) == std::tolower(b);
        });
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

    if (args.count("tx_block_size") != 0)
    {
        current_tx_block_size = std::stoi(args.at("tx_block_size"));
    }
    else
    {
        current_tx_block_size = DEFAULT_TX_BUFFER_LENGTH;
    }
    SoapySDR_logf(SOAPY_SDR_INFO, "TX block size set to %u", current_tx_block_size);

    /* set the source to what is passed in */
    if (args.count("clock_source") > 0) 
    {
        setClockSource(args.at("clock_source"));
    }

    if (args.count("time_source") > 0) 
    {
        setTimeSource(args.at("time_source"));
    }


    rx_hdl = skiq_rx_hdl_A1;
    tx_hdl = skiq_tx_hdl_A1;

    skiq_xport_type_t type  = skiq_xport_type_auto;
    skiq_xport_init_level_t level = skiq_xport_init_level_full;

    SoapySDR_logf(SOAPY_SDR_INFO, "Sidekiq opening card %u", card);

    /* init sidekiq */
    status = skiq_init(type, level, &card, 1);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_init failed (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
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

    setSampleRate(SOAPY_SDR_RX, DEFAULT_CHANNEL, static_cast<uint32_t>(this->rx_sample_rate));
    setSampleRate(SOAPY_SDR_TX, DEFAULT_CHANNEL, static_cast<uint32_t>(this->tx_sample_rate));

    this->rx_bandwidth = DEFAULT_BANDWIDTH;
    this->tx_bandwidth = DEFAULT_BANDWIDTH;

    setBandwidth(SOAPY_SDR_RX, DEFAULT_CHANNEL, static_cast<uint32_t>(this->rx_bandwidth));
    setBandwidth(SOAPY_SDR_TX, DEFAULT_CHANNEL, static_cast<uint32_t>(this->tx_bandwidth));

    /* set default frequency */
    this->rx_center_frequency = DEFAULT_FREQUENCY;
    setFrequency(SOAPY_SDR_RX, DEFAULT_CHANNEL, static_cast<uint64_t>(this->rx_center_frequency)); 

    this->tx_center_frequency = DEFAULT_FREQUENCY;
    setFrequency(SOAPY_SDR_TX, DEFAULT_CHANNEL, static_cast<uint64_t>(this->rx_center_frequency)); 

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

    status = skiq_read_num_tx_chans(card, &channels);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_num_tx_chans failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }
    num_tx_channels = channels;
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
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", dir);
        throw std::runtime_error("");
    }

    return -1;
}

/*******************************************************************
 * Antenna API
 ******************************************************************/

std::vector<std::string> SoapySidekiq::listAntennas(const int direction, const size_t channel) const {
    std::vector<std::string> antennas;

    SoapySDR_logf(SOAPY_SDR_TRACE, "listAntennas");
    
    if (direction == SOAPY_SDR_RX)
    {
        if (channel >= skiq_rx_hdl_end)
        {
            antennas.push_back("NONE");
        }
        else
        {
            if (this->param.rx_param[channel].num_trx_rf_ports > 0)
            {
                antennas.push_back("TRX");

            }
            else if (this->param.rx_param[channel].num_fixed_rf_ports > 0)
            {
                antennas.push_back("RX");
            }
            else
            {
                antennas.push_back("NONE");
            }
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        if (channel >= skiq_tx_hdl_end)
        {
            antennas.push_back("NONE");
        }
        else
        {
            if (this->param.tx_param[channel].num_trx_rf_ports > 0)
            {
                antennas.push_back("TRX");

            }
            else if (this->param.tx_param[channel].num_fixed_rf_ports > 0)
            {
                antennas.push_back("TX");
            }
            else
            {
                antennas.push_back("NONE");
            }
        }
    }


    return antennas;
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
        status = skiq_read_rx_cal_types_avail(card, this->rx_hdl, &mask);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_rx_cal_types_avail failed, "
                          "(card %u, handle %u, mask %d), status %d",
                          card, rx_hdl, mask, status);
            throw std::runtime_error("");
        }

        if ((mask & skiq_rx_cal_type_dc_offset) == skiq_rx_cal_type_dc_offset)
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, handle %u, has DC Offest correction, " 
                          "mask is: %d",
                          card, this->rx_hdl, mask);
            return true;
        }
        else
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, handle %u, does not have DC Offest correction, " 
                          "mask is: %d",
                          card, this->rx_hdl, mask);
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
        this->rx_hdl = static_cast<skiq_rx_hdl_t>(channel);

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
        status = skiq_read_rx_cal_mode(card, this->rx_hdl, &cal_mode);
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

std::vector<std::string> SoapySidekiq::listGains(const int direction, const size_t channel) const {
    //  list available gain elements,
    std::vector<std::string> results;
    SoapySDR_logf(SOAPY_SDR_TRACE, "listGains");
    results.push_back("LNA");
    return results;
}

// the Gain API is called for tx attenuation too.
bool SoapySidekiq::hasGainMode(const int direction, const size_t channel) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "hasGainMode");

    // all Sidekiq cards have rx gain mode and tx attenuation mode
    return true;
}

void SoapySidekiq::setGainMode(const int direction, const size_t channel,
                               const bool automatic)
{
    int status = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setGainMode");

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = static_cast<skiq_rx_hdl_t>(channel);

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

        status = skiq_read_rx_gain_mode(card, this->rx_hdl, &p_gain_mode);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_gain_mode failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, handle: %u, RX gain mode: is %s",
                      card, this->rx_hdl, p_gain_mode ? "skiq_rx_gain_auto" :
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

void SoapySidekiq::setGain(const int direction, const size_t channel, const std::string &name, const double value)
{
    setGain(direction, channel, value);
}

void SoapySidekiq::setGain(const int direction, const size_t channel, const double value)
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "setGain called with direction %d, channel %zu, value %.1f", direction, channel, value);

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = static_cast<skiq_rx_hdl_t>(channel);

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
            SoapySDR_logf(SOAPY_SDR_INFO, "Gain mode was auto, switching to manual for explicit gain setting.");
            status = skiq_write_rx_gain_mode(card, this->rx_hdl, skiq_rx_gain_manual);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                    "skiq_write_rx_gain_mode failed (card %u, channel %zu), status %d",
                    card, channel, status);
                throw std::runtime_error("");
            }
        }

        // 3. Query gain index range from hardware
        uint8_t gain_min = 0, gain_max = 0;
        status = skiq_read_rx_gain_index_range(card, this->rx_hdl, &gain_min, &gain_max);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_read_rx_gain_index_range failed (card %u, channel %zu), status %d",
                card, channel, status);
            throw std::runtime_error("");
        }

        // 4. Map value in dB to hardware index per device type
        uint8_t gain_index = gain_min;
        switch (part)
        {
            case skiq_mpcie:
            case skiq_m2:
            case skiq_m2_2280:
            case skiq_z2:
            case skiq_z3u:
                gain_index = (uint8_t)std::round(value); // 1dB/step, 0..76
                break;
            case skiq_x4:
            case skiq_x2:
                gain_index = (uint8_t)(195 + std::round(value * 2.0)); // 0.5dB/step, starts at 195
                break;
            case skiq_nv100:
                gain_index = (uint8_t)(187 + std::round(value * 2.0)); // 0.5dB/step, starts at 187
                break;
            default:
                SoapySDR_logf(SOAPY_SDR_WARNING, "Unknown card type: %u. Not setting gain.", (uint8_t)part);
                return;
        }

        // 5. Clamp gain index to allowed range
        if (gain_index < gain_min) gain_index = gain_min;
        if (gain_index > gain_max) gain_index = gain_max;

        SoapySDR_logf(SOAPY_SDR_INFO,
            "card: %u, handle: %u, Set RX gain: requested %.1f dB (gain_index: %u), range: [%u-%u], set: %u",
            card, this->rx_hdl, value, gain_index, gain_min, gain_max, gain_index);

        // 6. Actually set the gain
        status = skiq_write_rx_gain(card, rx_hdl, gain_index);
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
        // For completeness, update TX as you already do...
        uint16_t attenuation_index = 0;
        uint32_t max_attenuation_index = this->param.tx_param[tx_hdl].atten_quarter_db_max;

        switch (part)
        {
            case skiq_mpcie:
            case skiq_m2:
            case skiq_m2_2280:
            case skiq_z2:
            case skiq_z3u:
                if ((value < 0) || (value > 89.75))
                {
                    SoapySDR_logf(SOAPY_SDR_WARNING,
                        "card: %u, invalid requested attenuation: %3.0f dB, acceptable range: 0 - 89.75 dB. No attenuation configured.",
                        card, value);
                    return;
                }
                attenuation_index = max_attenuation_index - (uint16_t)std::round(value * 4.0);
                break;

            case skiq_x4:
            case skiq_x2:
            case skiq_nv100:
                if ((value < 0) || (value > 41.75))
                {
                    SoapySDR_logf(SOAPY_SDR_WARNING,
                        "card: %u, invalid requested attenuation: %3.0f dB, acceptable range: 0 - 41.75 dB. No attenuation configured.",
                        card, value);
                    return;
                }
                attenuation_index = max_attenuation_index - (uint16_t)std::round(value * 4.0);
                break;

            default:
                SoapySDR_logf(SOAPY_SDR_WARNING, "Unknown card type: %u. Not setting TX attenuation.", (uint8_t)part);
                return;
        }

        status = skiq_write_tx_attenuation(card, tx_hdl, attenuation_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_write_tx_gain failed, (card %u, value %f), status %d",
                card, attenuation_index, status);
            throw std::runtime_error("");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, Setting tx attenuation: %2.2f dB, attenuation index: %d",
            card, value, attenuation_index);
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }
}

double SoapySidekiq::getGain(const int direction, const size_t channel, const std::string &name) const
{
    return getGain(direction, channel);
}

double SoapySidekiq::getGain(const int direction, const size_t channel) const
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "getGain, direction: %d channel: %zu", direction, channel);

    if (direction == SOAPY_SDR_RX)
    {
        uint8_t gain_index;
        status = skiq_read_rx_gain(card, static_cast<skiq_rx_hdl_t>(channel), &gain_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_read_rx_gain failed (card %u), rx_hdl %zu, status %d",
                card, channel, status);
            throw std::runtime_error("");
        }

        switch (part)
        {
            case skiq_mpcie:
            case skiq_m2:
            case skiq_m2_2280:
            case skiq_z2:
            case skiq_z3u:
                return static_cast<double>(gain_index); // 1dB/step
            case skiq_x2:
            case skiq_x4:
                return static_cast<double>(gain_index - 195) / 2.0; // 0.5dB/step
            case skiq_nv100:
                return static_cast<double>(gain_index - 187) / 2.0;
            default:
                SoapySDR_logf(SOAPY_SDR_WARNING, "card: %u, invalid card type %u", card, (uint8_t)part);
                break;
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        uint16_t attenuation_index = 0;
        uint32_t max_attenuation_index = this->param.tx_param[tx_hdl].atten_quarter_db_max;

        status = skiq_read_tx_attenuation(card, tx_hdl, &attenuation_index);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "skiq_read_tx_attenuation failed (card %u), status %d",
                card, status);
            throw std::runtime_error("");
        }

        return (max_attenuation_index - static_cast<int>(attenuation_index)) / 4.0;
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
    return getGainRange(direction, channel);
}


SoapySDR::Range SoapySidekiq::getGainRange(const int    direction,
                                           const size_t channel) const
{
    SoapySDR_log(SOAPY_SDR_TRACE, "getGainRange");

    if (direction == SOAPY_SDR_RX)
    {
        double gain_min = 0;
        double gain_max = 0;
        double step = 0;


        // convert index to  dB based upon the card type
        switch (part)
        {
            // 0 to 76 [0 to 76 dB, 1 dB/step]
            case skiq_mpcie:
            case skiq_m2:
            case skiq_m2_2280:
            case skiq_z2:
            case skiq_z3u:
                gain_min = 0;
                gain_max = 76;
                step = 1;
                break;

            // 195 to 255 [0 to 30 dB, 0.5 dB/step]
            case skiq_x2:
            case skiq_x4:
                gain_min = 0;
                gain_max = 30;
                step = 0.5;
                break;

            // 187 to 255 [0 to 34 dB, 0.5 dB/step]
            case skiq_nv100:
                gain_min = 0;
                gain_max = 34;
                step = 0.5;
                break;

            default:
                SoapySDR_logf(SOAPY_SDR_WARNING,
                        "card: %u, invalid card type %u",
                        card, (uint8_t)part);
                break;
        }

        return SoapySDR::Range(gain_min, gain_max, step);
    }
    else if (direction == SOAPY_SDR_TX)
    {
        double attenuation_min = 0;
        double attenuation_max = 0;
        double attenuation_step = 0;

        // convert index to  dB based upon the card type
        switch (part)
        {
            case skiq_mpcie:
            case skiq_m2:
            case skiq_m2_2280:
            case skiq_z2:
            case skiq_z3u:
                attenuation_max = 89.75;
                attenuation_step = 0.25;
                break;

            case skiq_x2:
            case skiq_x4:
            case skiq_nv100:
                attenuation_max = 41.75;
                attenuation_step = 0.25;
                break;

            default:
                SoapySDR_logf(SOAPY_SDR_WARNING,
                              "card: %u, invalid card type %u",
                              card, (uint8_t)part);
                break;
        }
        return SoapySDR::Range(attenuation_min, attenuation_max, attenuation_step);
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

void SoapySidekiq::setFrequency(const int direction, const size_t channel,
                  const double frequency, const SoapySDR::Kwargs &args)
{
    int status = 0;

    SoapySDR_log(SOAPY_SDR_TRACE, "setFrequency");

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = static_cast<skiq_rx_hdl_t>(channel);
        this->rx_center_frequency = (uint64_t)frequency;

        SoapySDR_logf(SOAPY_SDR_INFO, "Setting rx center freq: %lu",
                      rx_center_frequency);

        status = skiq_write_rx_LO_freq(this->card, this->rx_hdl, rx_center_frequency);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_rx_LO_freq failed, (card %u, frequency "
                          "%lu), status %d",
                          this->card, rx_center_frequency, status);
            throw std::runtime_error("");
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        this->tx_hdl = static_cast<skiq_tx_hdl_t>(channel);
        this->tx_center_frequency = (uint64_t)frequency;

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
        status = skiq_read_rx_LO_freq(card, rx_hdl, &freq, &tuned_freq);
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
        status = skiq_read_tx_LO_freq(card, tx_hdl, &freq, &tuned_freq);
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

void SoapySidekiq::setSampleRate(const int direction, const size_t channel,
                                 const double rate)
{
    int status = 0;

    SoapySDR_log(SOAPY_SDR_TRACE, "setSampleRate");

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = static_cast<skiq_rx_hdl_t>(channel);
        this->rx_sample_rate = (uint32_t)rate;

        status = skiq_write_rx_sample_rate_and_bandwidth(this->card,
                                                         this->rx_hdl,
                                                         rx_sample_rate,
                                                         this->rx_bandwidth);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_rx_sample_rate_and_bandwidth "
                          "(card %u, sample_rate %u, bandwidth %u, status %d)",
                          this->card, rx_sample_rate, this->rx_bandwidth, status);
            throw std::runtime_error("");
        }

        SoapySDR_logf(SOAPY_SDR_INFO, "set rx sample rate: %u", rx_sample_rate);

        // Validate that the sample rate was set to what was desired.
        // otherwise log a warning
        uint32_t actual_rate;
        double fpga_rate;
        uint32_t actual_bw;
        uint32_t fpga_bw;

        status = skiq_read_rx_sample_rate_and_bandwidth(this->card,
                                                        this->rx_hdl,
                                                        &actual_rate,
                                                        &fpga_rate,
                                                        &actual_bw,
                                                        &fpga_bw);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "card: %u, skiq_read_rx_sample_rate_and_bandwidth "
                          "failed, status: %d",
                          this->card, status);
            throw std::runtime_error("");
        }

        if (rx_sample_rate != actual_rate)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING, "requested RX rate: %u, is not the same as "
                          "the actual rate: %u",
                          rx_sample_rate, actual_rate);
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        this->tx_hdl = static_cast<skiq_tx_hdl_t>(channel);
        this->tx_sample_rate = (uint32_t)rate;

        status = skiq_write_tx_sample_rate_and_bandwidth(this->card,
                                                         this->tx_hdl,
                                                         tx_sample_rate,
                                                         this->tx_bandwidth);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_tx_sample_rate_and_bandwidth failed, "
                          "(card %u, sample_rate %u, bandwidth %u, status %d)",
                          this->card, tx_sample_rate, tx_bandwidth, status);
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
        status = skiq_read_rx_sample_rate(card, rx_hdl, &rate, &actual_rate);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                           "skiq_read_rx_sample_rate failed (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        return static_cast<double>(rate);
    }
    else if (direction == SOAPY_SDR_TX)
    {
        status = skiq_read_tx_sample_rate(card, tx_hdl, &rate, &actual_rate);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_sample_rate failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        return static_cast<double>(rate);
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
    int                 status = 0;
    uint32_t            min_sample_rate;
    uint32_t            max_sample_rate;
    SoapySDR::RangeList ranges;

    SoapySDR_log(SOAPY_SDR_TRACE, "getSampleRateRange");

    status = skiq_read_min_sample_rate(card, &min_sample_rate);
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
                      "skiq_read_max_sample_rate (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }
    ranges.push_back(SoapySDR::Range(min_sample_rate, max_sample_rate));

    return ranges;
}

void SoapySidekiq::setBandwidth(const int direction, const size_t channel,
                                const double bw)
{
    int status = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setBandwidth");

    if (direction == SOAPY_SDR_RX)
    {
        this->rx_hdl = static_cast<skiq_rx_hdl_t>(channel);
        this->rx_bandwidth = (uint32_t)bw;

        status       = skiq_write_rx_sample_rate_and_bandwidth(this->card,
                                                               this->rx_hdl,
                                                               this->rx_sample_rate,
                                                               rx_bandwidth);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_rx_sample_rate_and_bandwidth failed "
                          "(card %u, sample_rate %u, bandwidth %u, status %d)",
                          this->card, this->rx_sample_rate, rx_bandwidth, status);
            throw std::runtime_error("");
        }

        SoapySDR_logf(SOAPY_SDR_INFO, "set rx bandwidth to %u", rx_bandwidth);

        // validate that the bandwidth was set to what was desired.
        // otherwise log a warning
        uint32_t actual_rate;
        double fpga_rate;
        uint32_t actual_bw;
        uint32_t fpga_bw;

        status = skiq_read_rx_sample_rate_and_bandwidth(this->card,
                                                        this->rx_hdl,
                                                        &actual_rate,
                                                        &fpga_rate,
                                                        &actual_bw,
                                                        &fpga_bw);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_sample_rate_and_bandwidth failed "
                          "(card %u, sample_rate %u, bandwidth %u, status %u)",
                          this->card, this->rx_sample_rate, rx_bandwidth, status);
            throw std::runtime_error("");
        }

        if (rx_bandwidth != actual_bw)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING, "requested bandwidth: %u, is not the same as "
                          "actual bandwidth: %u",
                          rx_bandwidth, actual_bw);
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        this->tx_hdl = static_cast<skiq_tx_hdl_t>(channel);
        this->tx_bandwidth = (uint32_t)bw;

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
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }
}

std::vector<double> SoapySidekiq::listSampleRates(const int direction, const size_t channel) const {
  std::vector<double> results;

  uint32_t min_sample_rate;
  if (skiq_read_min_sample_rate(card, &min_sample_rate) != 0) {
    SoapySDR_logf(SOAPY_SDR_ERROR, "Failure: skiq_read_min_sample_rate (card %d)", card);
  }
  uint32_t max_sample_rate;
  if (skiq_read_max_sample_rate(card, &max_sample_rate) != 0) {
    SoapySDR_logf(SOAPY_SDR_ERROR, "Failure: skiq_read_min_sample_rate (card %d)", card);
  }

  //  iterate through all sample rates
  uint32_t sample_rate = min_sample_rate;
  while (sample_rate <= max_sample_rate) {
    results.push_back(sample_rate);
    sample_rate += 250000;
  }

  return results;
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
        status = skiq_read_rx_sample_rate_and_bandwidth(card, rx_hdl, &rate,
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
        status = skiq_read_tx_sample_rate_and_bandwidth(card, tx_hdl, &rate,
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

    return bandwidth;
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
        if (value == "true")
        {
            status = skiq_write_rx_data_src(this->card,
                                            this->rx_hdl,
                                            skiq_data_src_counter);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_write_rx_data_src failed, card: %u status: %d",
                              this->card, status);
                throw std::runtime_error("");
            }
            else
            {
                SoapySDR_log(SOAPY_SDR_INFO, "set rx src to counter mode ");
                counter = true;
            }
        }
        else
        {
            status = skiq_write_rx_data_src(card, rx_hdl, skiq_data_src_iq);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_write_rx_data_src failed, card: %u status: %d",
                              card, status);
                throw std::runtime_error("");
            }
            else
            {
                SoapySDR_log(SOAPY_SDR_INFO, "set rx src to normal, not counter, mode ");
                counter = false;
            }
        }
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

