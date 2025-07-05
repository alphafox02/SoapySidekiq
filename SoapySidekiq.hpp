#pragma once

#include <sidekiq_api.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>


#define DEFAULT_CHANNEL 0
#define DEFAULT_SAMPLE_RATE (20000000)
#define DEFAULT_BANDWIDTH (18000000)
#define DEFAULT_FREQUENCY (1000000000)
#define DEFAULT_NUM_BUFFERS (30000)
#define DEFAULT_TX_BUFFER_LENGTH (16380)
#define DEFAULT_SLEEP_US (1)
#define SLEEP_1SEC (1 * 1000000)
#define NANOS_IN_SEC (1000000000ULL)


class SoapySidekiq : public SoapySDR::Device
{
    public:
        SoapySidekiq(const SoapySDR::Kwargs &args);

        ~SoapySidekiq(void);

        /*******************************************************************
         * Identification API
         ******************************************************************/

        std::string getDriverKey(void) const;

        std::string getHardwareKey(void) const;


        SoapySDR::Kwargs getHardwareInfo(void) const;

        /*******************************************************************
         * Channels API
         ******************************************************************/

        size_t getNumChannels(const int) const;

        /*******************************************************************
         * Stream API
         ******************************************************************/

        std::vector<std::string> getStreamFormats(const int    direction,
                const size_t channel) const;

        std::string getNativeStreamFormat(const int direction, const size_t channel,
                double &fullScale) const;

        SoapySDR::ArgInfoList getStreamArgsInfo(const int    direction,
                const size_t channel) const;

        SoapySDR::Stream *setupStream(const int direction,
                const std::string &format,
                const std::vector<size_t> &channels = std::vector<size_t>(),
                const SoapySDR::Kwargs &args = SoapySDR::Kwargs());

        void closeStream(SoapySDR::Stream *stream);

        size_t getStreamMTU(SoapySDR::Stream *stream) const;

        int activateStream(SoapySDR::Stream *stream,
                const int flags = 0,
                const long long timeNs = 0,
                const size_t numElems = 0);

        int deactivateStream(SoapySDR::Stream *stream,
                const int flags = 0,
                const long long timeNs = 0);

        int readStream(SoapySDR::Stream *stream,
                void *const *buffs,
                const size_t numElems,
                int &flags,
                long long &timeNs,
                const long timeoutUs = 100000);

        int writeStream(SoapySDR::Stream *stream,
                const void *const *buffs,
                const size_t numElems,
                int &flags,
                const long long timeNs = 0,
                const long timeoutUs = 100000);

        int readStreamStatus(SoapySDR::Stream *stream,
                size_t &chanMask,
                int &flags,
                long long &timeNs,
                const long timeoutUs = 100000);


        /*******************************************************************
         * Antenna API
         ******************************************************************/
  
        std::vector<std::string> listAntennas(const int direction, 
                const size_t channel  ) const;


        /*******************************************************************
         * Frontend corrections API
         ******************************************************************/

        bool hasDCOffsetMode(const int direction,
                const size_t channel) const;

        void setDCOffsetMode(const int direction,
                const size_t channel,
                const bool automatic);

        bool getDCOffsetMode(const int direction,
                const size_t channel) const;

        /*******************************************************************
         * Gain API
         ******************************************************************/

        std::vector<std::string> listGains(const int direction, 
                const size_t channel) const;

        bool hasGainMode(const int direction,
                const size_t channel) const;

        void setGainMode(const int direction,
                const size_t channel,
                const bool automatic);

        bool getGainMode(const int direction,
                const size_t channel) const;

        void setGain(const int direction,
                const size_t channel,
                const std::string &name,
                const double value);

        void setGain(const int direction,
                const size_t channel,
                const double value) override;
 
        double getGain(const int direction,
                const size_t channel,
                const std::string &name) const override;

        double getGain(const int direction,
                const size_t channel) const;

        SoapySDR::Range getGainRange(const int direction,
                const size_t channel,
                const std::string & name) const;

        SoapySDR::Range getGainRange(const int    direction,
                const size_t channel) const;

        /*******************************************************************
         * Frequency API
         ******************************************************************/
        void setFrequency(const int direction, const size_t channel,
                const double frequency,
                const SoapySDR::Kwargs &args = SoapySDR::Kwargs());

        double getFrequency(const int direction,
                const size_t channel) const;

        SoapySDR::RangeList getFrequencyRange(const int direction,
                const size_t channel) const;

        /*******************************************************************
         * Sample Rate API
         ******************************************************************/

        void setSampleRate(const int direction,
                const size_t channel,
                const double rate);

        double getSampleRate(const int direction,
                const size_t channel) const;

        SoapySDR::RangeList getSampleRateRange(const int    direction,
                const size_t channel) const;

        std::vector<double> listSampleRates(const int direction, const size_t channel) const override;

        void setBandwidth(const int direction,
                const size_t channel,
                const double bw);

        double getBandwidth(const int direction,
                const size_t channel) const;

        /*******************************************************************
         * Sensor API
         ******************************************************************/
        std::vector<std::string> listSensors(void) const;
        std::string readSensor(const std::string &key) const;

        /*******************************************************************
         * Settings API
         ******************************************************************/

        SoapySDR::ArgInfoList getSettingInfo(void) const;

        void writeSetting(const std::string &key,
                const std::string &value);

        std::string readSetting(const std::string &key) const;

        /*******************************************************************
         * Time API
         ******************************************************************/

        std::vector<std::string> listTimeSources(void) const;

        void setTimeSource(const std::string &source);

        std::string getTimeSource(void) const;

        bool hasHardwareTime(const std::string &) const;

        long long getHardwareTime(const std::string &) const;

        void setHardwareTime(const long long timeNs,
                const std::string &what);

        /*******************************************************************
         * Clocking API
         ******************************************************************/

        std::vector<std::string> listClockSources(void) const;

        void setClockSource(const std::string &source);

        std::string getClockSource(void) const;

        double getReferenceClockRate(void) const;



    private:
        long long convert_timestamp_to_nanos(const uint64_t timestamp, 
                                             const uint64_t timestamp_freq) const;

        SoapySDR::Stream *const TX_STREAM = (SoapySDR::Stream *)0x1;
        SoapySDR::Stream *const RX_STREAM = (SoapySDR::Stream *)0x2;

        //  sidekiq card
        std::string part_str;
        skiq_part_t part;
        skiq_param_t param;
        uint8_t card{};
        std::basic_string<char> serial{};
        std::basic_string<char> timeSource{};
        uint32_t resolution{};
        double maxValue{};

        bool rxUseShort{};
        bool txUseShort{};
        uint32_t debug_ctr{};

        //  rx
        std::mutex rx_mutex;
        std::condition_variable _cv;
        std::basic_string<char> timetype{};
        static bool rx_running;
        bool rx_receive_operation_exited_due_to_error{};

        uint8_t num_rx_channels{};
        skiq_rx_hdl_t rx_hdl{};
        uint64_t rx_center_frequency{};
        uint32_t rx_sample_rate{};
        uint32_t rx_bandwidth{};
        uint32_t rx_block_size_in_words{};
        uint32_t rx_block_size_in_bytes{};
        uint32_t rx_payload_size_in_bytes{};
        uint32_t rx_payload_size_in_words{};

        //  tx
        std::mutex tx_mutex;
        std::mutex tx_buf_mutex;
        pthread_mutex_t tx_enabled_mutex;
        pthread_cond_t tx_enabled_cond;
        pthread_mutex_t space_avail_mutex;
        pthread_cond_t space_avail_cond;
        bool space_avail{};
        int32_t *p_tx_status{};
        bool first_transmit{};

        uint8_t  num_tx_channels{};
        skiq_tx_hdl_t tx_hdl{};
        uint64_t tx_center_frequency{};
        uint32_t tx_sample_rate{};
        uint32_t tx_bandwidth{};
        uint32_t tx_underruns{};
        uint32_t complete_count{};
        uint32_t current_tx_block_size{};

        //  setting
        bool iq_swap{};
        bool counter{};
        bool log{};
        bool rfTimeSource{};
        uint64_t sys_freq{};

        // RX buffer
        skiq_rx_block_t *p_rx_block[DEFAULT_NUM_BUFFERS];
        uint32_t rxReadIndex{};
        uint32_t rxWriteIndex{};

        // Buffer for leftover RX samples to allow readStream() to return arbitrary numElems
        std::vector<int16_t> rx_fifo_buffer;
        size_t rx_fifo_offset = 0;

        // TX buffer
        skiq_tx_block_t *p_tx_block[DEFAULT_NUM_BUFFERS];
        uint32_t currTXBuffIndex{};
        uint32_t p_tx_block_index{};


        // TX callback static function
        // The registration requires a static function instead of a method so
        // this must be created to be able to register it.
        // This function calls the tx_complete method.
        static void static_tx_complete_callback(int32_t status, 
                                                skiq_tx_block_t *p_data, 
                                                void *p_user)
        {
            // cast the passed in void pointer to the structure that was passed.
            passedStruct  *instance = static_cast<passedStruct*>(p_user);

            // the structure contains the SoapySidekiq instance and the index of the block
            // that was transmitted
            SoapySidekiq *self = instance->classAddr;
            uint32_t txIndex = instance->txIndex;

            // Call the member function
            self->tx_complete(status, p_data, txIndex);

            delete instance;
        }

        // TX enabled callback static function
        // The registration requires a static function instead of a method so
        // this must be created to be able to register it.
        // This function calls the tx_enabled method.
        static void static_tx_enabled_callback(uint8_t card, int32_t status) 
        {
            // the structure contains the SoapySidekiq instance and the index of the block
            // that was transmitted
            SoapySidekiq *self = thisClassAddr;

            // Call the member function
            self->tx_enabled(card, status);
        }

        static SoapySidekiq *thisClassAddr;

    public:
        struct passedStruct
        {
            SoapySidekiq *classAddr;
            uint32_t txIndex;
        };

        passedStruct *passedStructInstance;

        //  receive thread
        std::thread _rx_receive_thread;
        void rx_receive_operation(void);
        void rx_receive_operation_impl(void);
        static std::vector<SoapySDR::Kwargs> sidekiq_devices;

        // tx thread
        std::thread _tx_streaming_thread;
        void tx_streaming_start();

        // tx callback method
        void tx_complete(int32_t status, skiq_tx_block_t *p_data, uint32_t txIndex);

        // tx enabled callback
        void tx_enabled(uint8_t card, int32_t status);
};


