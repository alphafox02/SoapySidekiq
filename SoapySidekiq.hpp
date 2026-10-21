/**
 * @file SoapySidekiq.hpp
 * @brief Declares the SoapySDR device implementation for Epiq Sidekiq radios.
 */

#pragma once

#include <sidekiq_api.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.h>
#include <SoapySidekiq/RxStreamSession.hpp>

inline constexpr int DEFAULT_SAMPLE_RATE = 20000000;
inline constexpr int DEFAULT_BANDWIDTH = 18000000;
inline constexpr int DEFAULT_FREQUENCY = 1000000000;
inline constexpr int DEFAULT_NUM_BUFFERS = 30000;
inline constexpr int DEFAULT_SLEEP_US = 1;
inline constexpr int SLEEP_1SEC = 1000000;
inline constexpr unsigned long long NANOS_IN_SEC = 1000000000ULL;

/**
 * @brief Owns a Sidekiq card's streams and callback state.
 *
 * Instances remain at a stable address while SDK callbacks and worker threads
 * refer to them. Copying and moving are explicitly disabled so these references
 * and the ownership of device resources cannot be transferred.
 */
class SoapySidekiq : public SoapySDR::Device
{
  public:
    SoapySidekiq(const SoapySDR::Kwargs &args);

    ~SoapySidekiq(void) override;

    SoapySidekiq(const SoapySidekiq &) = delete;
    SoapySidekiq &operator=(const SoapySidekiq &) = delete;
    SoapySidekiq(SoapySidekiq &&) = delete;
    SoapySidekiq &operator=(SoapySidekiq &&) = delete;

    /*******************************************************************
     * Identification API
     ******************************************************************/

    std::string getDriverKey(void) const override;

    std::string getHardwareKey(void) const override;

    SoapySDR::Kwargs getHardwareInfo(void) const override;

    /*******************************************************************
     * Channels API
     ******************************************************************/

    size_t getNumChannels(const int) const override;

    /*******************************************************************
     * Stream API
     ******************************************************************/

    std::vector<std::string> getStreamFormats(const int direction,
                                              const size_t channel) const override;

    std::string getNativeStreamFormat(const int direction, const size_t channel,
                                      double &fullScale) const override;

    SoapySDR::ArgInfoList getStreamArgsInfo(const int direction,
                                            const size_t channel) const override;

    SoapySDR::Stream *setupStream(const int direction, const std::string &format,
                                  const std::vector<size_t> &channels = std::vector<size_t>(),
                                  const SoapySDR::Kwargs &args = SoapySDR::Kwargs()) override;

    void closeStream(SoapySDR::Stream *stream) override;

    size_t getStreamMTU(SoapySDR::Stream *stream) const override;

    int activateStream(SoapySDR::Stream *stream, const int flags = 0, const long long timeNs = 0,
                       const size_t numElems = 0) override;

    int deactivateStream(SoapySDR::Stream *stream, const int flags = 0,
                         const long long timeNs = 0) override;

    int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems, int &flags,
                   long long &timeNs, const long timeoutUs = 100000) override;

    /**
     * Submit CS16 or normalized CF32 samples in whole TX MTUs, without staging.
     * numElems must fit int; invalid counts/buffers return SOAPY_SDR_STREAM_ERROR.
     * A zero-length write returns zero without accessing buffs. Back-pressure
     * returns an accepted sample prefix or SOAPY_SDR_TIMEOUT; callers retry
     * only the unaccepted suffix, advancing by two format components per sample.
     */
    int writeStream(SoapySDR::Stream *stream, const void *const *buffs, const size_t numElems,
                    int &flags, const long long timeNs = 0, const long timeoutUs = 100000) override;

    int readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags, long long &timeNs,
                         const long timeoutUs = 100000) override;

    /*******************************************************************
     * Antenna API
     ******************************************************************/

    std::vector<std::string> listAntennas(const int direction, const size_t channel) const override;
    std::string getAntenna(const int direction, const size_t channel) const override;
    void setAntenna(const int direction, const size_t channel, const std::string &name) override;

    /*******************************************************************
     * Frontend corrections API
     ******************************************************************/

    bool hasDCOffsetMode(const int direction, const size_t channel) const override;

    void setDCOffsetMode(const int direction, const size_t channel, const bool automatic) override;

    bool getDCOffsetMode(const int direction, const size_t channel) const override;

    /*******************************************************************
     * Gain API
     ******************************************************************/

    std::vector<std::string> listGains(const int direction, const size_t channel) const override;

    bool hasGainMode(const int direction, const size_t channel) const override;

    void setGainMode(const int direction, const size_t channel, const bool automatic) override;

    bool getGainMode(const int direction, const size_t channel) const override;

    void setGain(const int direction, const size_t channel, const std::string &name,
                 const double value) override;

    void setGain(const int direction, const size_t channel, const double value) override;

    double getGain(const int direction, const size_t channel,
                   const std::string &name) const override;

    double getGain(const int direction, const size_t channel) const override;

    SoapySDR::Range getGainRange(const int direction, const size_t channel,
                                 const std::string &name) const override;

    SoapySDR::Range getGainRange(const int direction, const size_t channel) const override;

    /*******************************************************************
     * Frequency API
     ******************************************************************/
    void setFrequency(const int direction, const size_t channel, const double frequency,
                      const SoapySDR::Kwargs &args = SoapySDR::Kwargs()) override;

    double getFrequency(const int direction, const size_t channel) const override;

    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel) const override;

    /*******************************************************************
     * Sample Rate API
     ******************************************************************/

    void setSampleRate(const int direction, const size_t channel, const double rate) override;

    double getSampleRate(const int direction, const size_t channel) const override;

    // Older supported SoapySDR APIs do not declare this virtual method.
#ifdef SOAPY_SDR_API_HAS_GET_SAMPLE_RATE_RANGE
    SoapySDR::RangeList getSampleRateRange(const int direction,
                                           const size_t channel) const override;
#else
    SoapySDR::RangeList getSampleRateRange(const int direction, const size_t channel) const;
#endif

    std::vector<double> listSampleRates(const int direction, const size_t channel) const override;

    void setBandwidth(const int direction, const size_t channel, const double bw) override;

    double getBandwidth(const int direction, const size_t channel) const override;

    /*******************************************************************
     * Sensor API
     ******************************************************************/
    std::vector<std::string> listSensors(void) const override;
    SoapySDR::ArgInfo getSensorInfo(const std::string &key) const override;
    std::string readSensor(const std::string &key) const override;

    /*******************************************************************
     * Settings API
     ******************************************************************/

    SoapySDR::ArgInfoList getSettingInfo(void) const override;

    void writeSetting(const std::string &key, const std::string &value) override;

    std::string readSetting(const std::string &key) const override;

    /*******************************************************************
     * Time API
     ******************************************************************/

    std::vector<std::string> listTimeSources(void) const override;

    void setTimeSource(const std::string &source) override;

    std::string getTimeSource(void) const override;

    bool hasHardwareTime(const std::string &) const override;

    long long getHardwareTime(const std::string &) const override;

    void setHardwareTime(const long long timeNs, const std::string &what) override;

    /*******************************************************************
     * Clocking API
     ******************************************************************/

    std::vector<std::string> listClockSources(void) const override;

    void setClockSource(const std::string &source) override;

    std::string getClockSource(void) const override;

    // Reference clock access was introduced in the SoapySDR 0.8 API.
#if defined(SOAPY_SDR_API_VERSION) && SOAPY_SDR_API_VERSION >= 0x00080000
    double getReferenceClockRate(void) const override;
#else
    double getReferenceClockRate(void) const;
#endif

    struct StreamHandle
    {
        int direction;
        size_t channel;
        skiq_rx_hdl_t rx_handle;
        skiq_tx_hdl_t tx_handle;
    };

  private:
    /** Resolve only this device's currently owned opaque stream tokens. */
    StreamHandle *findStreamHandle(SoapySDR::Stream *stream) const noexcept;

    /** Acquire TX callback resources; unwind acquired state on any failure. */
    void initializeTxCallbacks();

    /** Reject invalid directions/channels before metadata or SDK operations. */
    void validateChannel(int direction, size_t channel) const;
    /** Require a named gain element advertised for the selected channel. */
    void validateGainName(int direction, size_t channel, const std::string &name) const;
    /** Read the configured RF port; empty when reporting is unavailable. */
    std::string currentAntenna(int direction, size_t channel) const;
    /** RX index bounds and handle-specific conversion from the SDK gain table. */
    struct GainProfile
    {
        double rx_offset;
        double rx_steps_per_db;
        double rx_maximum;
    };
    GainProfile gainProfile(size_t channel) const;
    double txGainMaximum(size_t channel) const;
    /** Validated SDK configuration; caches change only after complete readback. */
    struct RateAndBandwidth
    {
        uint32_t configured_rate;
        double actual_rate;
        uint32_t actual_bandwidth;
    };
    /** Populate all initial RF caches only after every SDK readback is validated. */
    void initializeRateCaches();
    RateAndBandwidth readRateAndBandwidth(int direction, size_t channel) const;
    void applyRateAndBandwidth(int direction, size_t channel, uint32_t rate, uint32_t bandwidth);
    skiq_rx_hdl_t getRxHandle(const size_t channel) const;
    skiq_tx_hdl_t getTxHandle(const size_t channel) const;
    skiq_rx_hdl_t getFirstRxHandle(void) const;
    skiq_tx_hdl_t getFirstTxHandle(void) const;

    long long convert_timestamp_to_nanos(const uint64_t timestamp,
                                         const uint64_t timestamp_freq) const;

    //  sidekiq card
    std::string part_str;
    skiq_part_t part;
    skiq_param_t param;
    uint8_t card{};
    std::string serial{};
    std::string timeSource{};
    uint32_t resolution{};
    double maxValue{};   // RX ADC full scale.
    double txMaxValue{}; // TX host full scale accounts for DAC bit alignment.

    bool rxUseShort{};
    bool txUseShort{};

    // RX hardware adapter must outlive the session that references it.
    std::string timetype{};
    std::unique_ptr<soapy_sidekiq::RxStreamBackend> rx_backend;
    std::unique_ptr<soapy_sidekiq::RxStreamSession> rx_session;
    StreamHandle *active_rx_stream{};

    uint8_t num_rx_channels{};
    std::vector<uint32_t> rx_sample_rates;
    std::vector<uint32_t> rx_bandwidths;
    uint32_t rx_block_size_in_words{};
    uint32_t rx_block_size_in_bytes{};
    uint32_t rx_payload_size_in_bytes{};
    uint32_t rx_payload_size_in_words{};

    //  tx
    std::mutex tx_mutex;
    std::condition_variable tx_cv;
    std::mutex tx_buf_mutex;
    std::condition_variable tx_buf_cv;
    // Written with tx_mutex and tx_buf_mutex held; read with either held.
    bool tx_writes_stopping{true};
    std::size_t active_tx_writers{}; // Guarded by tx_buf_mutex.
    std::size_t active_tx_status_readers{}; // Guarded by tx_buf_mutex.
    /** Close write/status admission and drain admitted calls before stopping hardware. */
    void stopTxWrites();
    bool tx_start_finished{};
    int tx_start_status{};
    bool tx_stream_active{};
    // Fixed-size status storage is allocated before callbacks are registered.
    std::unique_ptr<int32_t[]> p_tx_status;
    bool first_transmit{};
    StreamHandle *active_tx_stream{};

    uint8_t num_tx_channels{};
    std::vector<uint32_t> tx_sample_rates;
    std::vector<uint32_t> tx_bandwidths;
    std::timed_mutex tx_status_mutex;
    uint32_t tx_underruns{};   // Guarded by tx_status_mutex; reset after successful SDK start.
    uint64_t complete_count{}; // Guarded by tx_buf_mutex.
    uint32_t current_tx_block_size{};

    //  setting
    bool iq_swap{};
    bool counter{};
    bool log{};
    bool rfTimeSource{};
    uint64_t sys_freq{};

    // TX buffer
    skiq_tx_block_t *p_tx_block[DEFAULT_NUM_BUFFERS]{};
    uint32_t currTXBuffIndex{};
    uint32_t p_tx_block_index{};

    // TX callback static function
    // The registration requires a static function instead of a method so
    // this must be created to be able to register it.
    // This function calls the tx_complete method.
    static void tx_complete_callback(int32_t status, skiq_tx_block_t *p_data,
                                     void *p_user) noexcept;

    static void registerInstance(uint8_t card, SoapySidekiq *instance);
    static void unregisterInstance(uint8_t card, SoapySidekiq *instance);
    static SoapySidekiq *getInstanceForCard(uint8_t card);

  public:
    /** Per-send context: owned by writeStream until accepted, then by completion. */
    struct passedStruct
    {
        SoapySidekiq *classAddr;
        uint32_t txIndex;
    };

    // tx thread
    std::thread _tx_streaming_thread;
    void tx_streaming_start(skiq_tx_hdl_t tx_handle);

    // tx callback method
    void tx_complete(int32_t status, skiq_tx_block_t *p_data, uint32_t txIndex);
};
