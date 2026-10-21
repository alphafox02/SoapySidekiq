/**
 * @file nv100_multichannel_rx.cpp
 * @brief Bounded receive-only NV100 counter validation for two-channel streams.
 */
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void require(bool value, const std::string &message)
{
    if (!value)
        throw std::runtime_error(message);
}

int main(int argc, char **argv)
{
    std::cout << std::unitbuf;
    std::unique_ptr<SoapySDR::Device, void (*)(SoapySDR::Device *)> device(
        nullptr, SoapySDR::Device::unmake);
    SoapySDR::Stream *stream = nullptr;
    std::string old_counter, old_time;
    try
    {
        require(argc <= 2, "usage: nv100_multichannel_rx [driver=sidekiq,card=0]");
        device.reset(SoapySDR::Device::make(argc == 2 ? argv[1] : "driver=sidekiq,card=0"));
        require(device && device->getHardwareKey() == "NV100", "NV100 unavailable");
        old_counter = device->readSetting("counter");
        old_time = device->readSetting("timetype");
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            if (std::abs(device->getSampleRate(SOAPY_SDR_RX, channel) - 5000000) >= 1)
            {
                device->setBandwidth(SOAPY_SDR_RX, channel, 4000000);
                device->setSampleRate(SOAPY_SDR_RX, channel, 5000000);
            }
            if (std::abs(device->getBandwidth(SOAPY_SDR_RX, channel) - 4000000) >= 1)
                device->setBandwidth(SOAPY_SDR_RX, channel, 4000000);
            device->setFrequency(SOAPY_SDR_RX, channel, 1800000000);
            require(std::abs(device->getSampleRate(SOAPY_SDR_RX, channel) - 5000000) < 1,
                    "unexpected rate");
        }
        device->writeSetting("counter", "true");
        for (const auto &channels :
             {std::vector<std::size_t>{1, 2}, {2, 1}, {0, 1, 2}, {0, 0}, {0, 3}})
        {
            bool rejected = false;
            try
            {
                stream = device->setupStream(SOAPY_SDR_RX, "CS16", channels);
            }
            catch (const std::logic_error &)
            {
                rejected = true;
            }
            if (stream)
            {
                device->closeStream(stream);
                stream = nullptr;
            }
            require(rejected, "invalid/conflicting channels accepted");
        }
        std::cout << "PASS: invalid, duplicate, and conflicting channel selections rejected\n";
        unsigned cases = 0;
        for (const auto &channels : {std::vector<std::size_t>{0, 1}, {1, 0}, {0, 2}, {2, 0}})
            for (const std::string format : {"CS16", "CF32"})
                for (const std::string time_source : {"rf_timestamp", "sys_timestamp"})
                {
                    std::cout << "BEGIN: pair=" << channels[0] << ',' << channels[1]
                              << " format=" << format << " time=" << time_source << '\n';
                    device->writeSetting("timetype", time_source);
                    stream = device->setupStream(SOAPY_SDR_RX, format, channels);
                    require(device->getStreamMTU(stream) > 0, "invalid MTU");
                    require(device->activateStream(stream) == 0, "activation failed");
                    constexpr std::size_t chunk = 257;
                    std::array<std::array<std::int16_t, chunk * 2>, 2> shorts{};
                    std::array<std::array<float, chunk * 2>, 2> floats{};
                    void *buffers[]{format == "CS16" ? static_cast<void *>(shorts[0].data())
                                                     : static_cast<void *>(floats[0].data()),
                                    format == "CS16" ? static_cast<void *>(shorts[1].data())
                                                     : static_cast<void *>(floats[1].data())};
                    std::array<std::uint16_t, 2> expected{};
                    std::uint16_t relative_phase{};
                    bool have_previous = false, have_phase = false;
                    long long expected_time = 0;
                    const double clock = std::stod(device->readSetting("sys_clock_freq"));
                    const auto tolerance = time_source == "rf_timestamp"
                                               ? 0LL
                                               : static_cast<long long>(std::ceil(1e9 / clock)) + 1;
                    std::size_t total = 0;
                    unsigned overflows = 0;
                    auto check = [&](int count, int flags, long long timestamp)
                    {
                        require(count > 0 && (flags & SOAPY_SDR_HAS_TIME),
                                "missing sample count/timestamp");
                        if (have_previous)
                            require(std::llabs(timestamp - expected_time) <= tolerance,
                                    "partial timestamp discontinuity");
                        for (int sample = 0; sample < count; ++sample)
                        {
                            std::array<std::uint16_t, 2> q{};
                            for (std::size_t ch = 0; ch < 2; ++ch)
                            {
                                const auto i = format == "CS16"
                                                   ? shorts[ch][2 * sample]
                                                   : static_cast<std::int16_t>(std::lround(
                                                         floats[ch][2 * sample] * 32767));
                                const auto quadrature =
                                    format == "CS16" ? shorts[ch][2 * sample + 1]
                                                     : static_cast<std::int16_t>(std::lround(
                                                           floats[ch][2 * sample + 1] * 32767));
                                q[ch] = static_cast<std::uint16_t>(quadrature);
                                require(static_cast<std::uint16_t>(i) ==
                                            static_cast<std::uint16_t>(q[ch] + 1),
                                        "IQ order/counter mismatch");
                                if (have_previous)
                                    require(q[ch] == expected[ch], "channel counter discontinuity");
                                expected[ch] = static_cast<std::uint16_t>(q[ch] + 2);
                            }
                            const auto phase = static_cast<std::uint16_t>(q[1] - q[0]);
                            if (have_phase)
                                require(phase == relative_phase, "channel alignment changed");
                            relative_phase = phase;
                            have_phase = have_previous = true;
                        }
                        expected_time = timestamp + count * 200LL;
                        total += static_cast<std::size_t>(count);
                    };
                    for (unsigned reads = 0; reads < 2000 && total < 65792; ++reads)
                    {
                        int flags = 0;
                        long long timestamp = 0;
                        const int count =
                            device->readStream(stream, buffers, chunk, flags, timestamp, 250000);
                        if (count == SOAPY_SDR_OVERFLOW)
                        {
                            ++overflows;
                            have_previous = false;
                            continue;
                        }
                        check(count, flags, timestamp);
                    }
                    require(total >= 65792, "insufficient verified samples");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    int flags = 99;
                    long long timestamp = 99;
                    require(device->readStream(stream, buffers, chunk, flags, timestamp, 250000) ==
                                SOAPY_SDR_OVERFLOW,
                            "stall did not report shared overflow");
                    require(flags == 0 && timestamp == 0, "overflow returned stale time metadata");
                    have_previous = false;
                    bool recovered = false;
                    for (unsigned retry = 0; retry < 16; ++retry)
                    {
                        const int count =
                            device->readStream(stream, buffers, chunk, flags, timestamp, 250000);
                        if (count == SOAPY_SDR_OVERFLOW)
                            continue;
                        check(count, flags, timestamp);
                        recovered = true;
                        break;
                    }
                    require(recovered, "stall recovery failed");
                    require(device->deactivateStream(stream) == 0, "stop failed");
                    require(device->activateStream(stream) == 0, "reactivation failed");
                    have_previous = have_phase = false;
                    bool restarted = false;
                    for (unsigned retry = 0; retry < 16; ++retry)
                    {
                        const int count =
                            device->readStream(stream, buffers, chunk, flags, timestamp, 250000);
                        if (count == SOAPY_SDR_OVERFLOW)
                            continue;
                        check(count, flags, timestamp);
                        restarted = true;
                        break;
                    }
                    require(restarted, "reactivation read failed");
                    require(device->deactivateStream(stream) == 0, "second stop failed");
                    device->closeStream(stream);
                    stream = nullptr;
                    ++cases;
                    std::cout << "PASS: pair=" << channels[0] << ',' << channels[1]
                              << " format=" << format << " time=" << time_source
                              << " samples_per_channel=" << total
                              << " initial_overflows=" << overflows
                              << " counter_delta=" << relative_phase
                              << " continuity/alignment/stall/recovery/reactivation verified\n";
                }
        device->writeSetting("counter", old_counter);
        device->writeSetting("timetype", old_time);
        std::cout << "PASS: " << cases
                  << " NV100 multi-channel RX cases; no TX or analog phase claim\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (device)
        {
            if (stream)
            {
                try
                {
                    device->deactivateStream(stream);
                }
                catch (...)
                {
                }
                try
                {
                    device->closeStream(stream);
                }
                catch (...)
                {
                }
            }
            if (!old_counter.empty())
            {
                try
                {
                    device->writeSetting("counter", old_counter);
                }
                catch (...)
                {
                }
            }
            if (!old_time.empty())
            {
                try
                {
                    device->writeSetting("timetype", old_time);
                }
                catch (...)
                {
                }
            }
        }
        return 1;
    }
}
