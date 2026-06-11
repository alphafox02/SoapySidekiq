#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::vector<size_t> parseChannels(const std::string &text)
{
    std::vector<size_t> channels;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        if (!item.empty()) channels.push_back(static_cast<size_t>(std::stoul(item)));
    }
    if (channels.empty()) throw std::runtime_error("at least one RX channel is required");
    return channels;
}

bool validateCounter(const int16_t *buffer,
                     const size_t values,
                     const int fullscale,
                     const std::string &label)
{
    int expected = buffer[0];
    for (size_t index = 0; index < values; index++)
    {
        const int value = buffer[index];
        if (value != expected)
        {
            std::cerr << label << ": bad value at " << index
                      << ", expected " << expected
                      << ", value " << value << "\n";
            const size_t begin = index > 5 ? index - 5 : 0;
            const size_t end = std::min(values, index + 5);
            for (size_t cursor = begin; cursor < end; cursor++)
            {
                std::cerr << "  [" << cursor << "] " << buffer[cursor] << "\n";
            }
            return false;
        }

        expected++;
        if (expected == fullscale + 1) expected = -(fullscale + 1);
    }

    return true;
}

struct Options
{
    std::string card = "0";
    std::vector<size_t> channels = {0, 1};
    double rate = 20e6;
    double bandwidth = 16e6;
    double frequency = 1000e6;
    int blocks = 4;
    int reads = 10;
    long timeoutUs = 10000000;
    int logLevel = SOAPY_SDR_WARNING;
};

Options parseArgs(int argc, char **argv)
{
    Options options;
    for (int index = 1; index < argc; index++)
    {
        const std::string arg = argv[index];
        const auto next = [&]() -> std::string {
            if (++index >= argc) throw std::runtime_error("missing value for " + arg);
            return argv[index];
        };

        if (arg == "-c" || arg == "--card") options.card = next();
        else if (arg == "--channels") options.channels = parseChannels(next());
        else if (arg == "-s" || arg == "--rate") options.rate = std::stod(next());
        else if (arg == "-bw" || arg == "--bandwidth") options.bandwidth = std::stod(next());
        else if (arg == "-f" || arg == "--frequency") options.frequency = std::stod(next());
        else if (arg == "--blocks") options.blocks = std::stoi(next());
        else if (arg == "--reads") options.reads = std::stoi(next());
        else if (arg == "--timeout-us") options.timeoutUs = std::stol(next());
        else if (arg == "--log-level") options.logLevel = std::stoi(next());
        else throw std::runtime_error("unknown argument " + arg);
    }
    return options;
}
}

int main(int argc, char **argv)
{
    try
    {
        const Options options = parseArgs(argc, argv);
        SoapySDR::setLogLevel(static_cast<SoapySDRLogLevel>(options.logLevel));

        SoapySDR::Kwargs kwargs;
        kwargs["driver"] = "sidekiq";
        kwargs["card"] = options.card;

        SoapySDR::Device *device = SoapySDR::Device::make(kwargs);
        if (device == nullptr) throw std::runtime_error("SoapySDR::Device::make failed");

        try
        {
            device->writeSetting("counter", "true");
            device->writeSetting("iq_swap", "false");
            device->writeSetting("timetype", "rf_timestamp");

            for (const auto channel : options.channels)
            {
                device->setSampleRate(SOAPY_SDR_RX, channel, options.rate);
                device->setBandwidth(SOAPY_SDR_RX, channel, options.bandwidth);
                device->setFrequency(SOAPY_SDR_RX, channel, options.frequency);
                device->setGainMode(SOAPY_SDR_RX, channel, true);
            }

            SoapySDR::Stream *stream =
                device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, options.channels);
            const size_t mtu = device->getStreamMTU(stream);
            const size_t count = mtu * static_cast<size_t>(options.blocks);
            double fullScaleValue = 0;
            device->getNativeStreamFormat(SOAPY_SDR_RX, 0, fullScaleValue);
            const int fullscale = static_cast<int>(fullScaleValue);

            std::vector<std::vector<int16_t>> buffers(
                options.channels.size(),
                std::vector<int16_t>(2 * count));
            std::vector<void *> bufferPtrs;
            for (auto &buffer : buffers) bufferPtrs.push_back(buffer.data());

            device->setHardwareTime(0, "now");
            device->activateStream(stream);
            try
            {
                for (int readIndex = 0; readIndex < options.reads; readIndex++)
                {
                    int flags = 0;
                    long long timeNs = 0;
                    const int ret = device->readStream(
                        stream,
                        bufferPtrs.data(),
                        count,
                        flags,
                        timeNs,
                        options.timeoutUs);
                    if (ret != static_cast<int>(count))
                    {
                        throw std::runtime_error("read returned " + std::to_string(ret) +
                                                 ", expected " + std::to_string(count));
                    }

                    bool ok = true;
                    for (size_t chanIndex = 0; chanIndex < options.channels.size(); chanIndex++)
                    {
                        ok = validateCounter(buffers[chanIndex].data(),
                                             buffers[chanIndex].size(),
                                             fullscale,
                                             "channel " + std::to_string(options.channels[chanIndex])) &&
                             ok;
                    }
                    if (!ok) return 1;

                    std::cout << "read " << readIndex << ": " << count
                              << " samples/channel channels=";
                    for (size_t index = 0; index < options.channels.size(); index++)
                    {
                        if (index != 0) std::cout << ",";
                        std::cout << options.channels[index];
                    }
                    std::cout << " timeNs=" << timeNs << "\n";
                }
            }
            catch (...)
            {
                device->deactivateStream(stream);
                device->closeStream(stream);
                device->writeSetting("counter", "false");
                throw;
            }

            device->deactivateStream(stream);
            device->closeStream(stream);
            device->writeSetting("counter", "false");
        }
        catch (...)
        {
            SoapySDR::Device::unmake(device);
            throw;
        }

        SoapySDR::Device::unmake(device);
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
