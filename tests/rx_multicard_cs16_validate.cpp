#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
template <typename T>
std::vector<T> parseList(const std::string &text)
{
    std::vector<T> values;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        if (!item.empty()) values.push_back(static_cast<T>(std::stoul(item)));
    }
    if (values.empty()) throw std::runtime_error("list argument cannot be empty");
    return values;
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
    std::vector<unsigned> cards = {0, 1};
    std::vector<size_t> channels = {0, 1};
    double rate = 5e6;
    double bandwidth = 4e6;
    double frequency = 1000e6;
    int blocks = 1;
    int reads = 3;
    long timeoutUs = 10000000;
    int logLevel = SOAPY_SDR_WARNING;
    bool startOn1pps = false;
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

        if (arg == "--cards") options.cards = parseList<unsigned>(next());
        else if (arg == "--channels") options.channels = parseList<size_t>(next());
        else if (arg == "-s" || arg == "--rate") options.rate = std::stod(next());
        else if (arg == "-bw" || arg == "--bandwidth") options.bandwidth = std::stod(next());
        else if (arg == "-f" || arg == "--frequency") options.frequency = std::stod(next());
        else if (arg == "--blocks") options.blocks = std::stoi(next());
        else if (arg == "--reads") options.reads = std::stoi(next());
        else if (arg == "--timeout-us") options.timeoutUs = std::stol(next());
        else if (arg == "--log-level") options.logLevel = std::stoi(next());
        else if (arg == "--start-on-1pps") options.startOn1pps = true;
        else throw std::runtime_error("unknown argument " + arg);
    }
    return options;
}

struct CardContext
{
    unsigned card = 0;
    SoapySDR::Device *device = nullptr;
    SoapySDR::Stream *stream = nullptr;
    size_t count = 0;
    int fullscale = 0;
    std::vector<std::vector<int16_t>> buffers;
    std::vector<void *> bufferPtrs;
};

void cleanup(CardContext &context)
{
    if (context.device != nullptr && context.stream != nullptr)
    {
        try
        {
            context.device->deactivateStream(context.stream);
        }
        catch (...) {}

        try
        {
            context.device->closeStream(context.stream);
        }
        catch (...) {}
        context.stream = nullptr;
    }

    if (context.device != nullptr)
    {
        try
        {
            context.device->writeSetting("counter", "false");
        }
        catch (...) {}
        SoapySDR::Device::unmake(context.device);
        context.device = nullptr;
    }
}
}

int main(int argc, char **argv)
{
    std::vector<CardContext> contexts;
    try
    {
        const Options options = parseArgs(argc, argv);
        SoapySDR::setLogLevel(static_cast<SoapySDRLogLevel>(options.logLevel));

        for (const auto card : options.cards)
        {
            CardContext context;
            context.card = card;

            SoapySDR::Kwargs kwargs;
            kwargs["driver"] = "sidekiq";
            kwargs["card"] = std::to_string(card);
            context.device = SoapySDR::Device::make(kwargs);
            if (context.device == nullptr)
            {
                throw std::runtime_error("SoapySDR::Device::make failed for card " +
                                         std::to_string(card));
            }

            context.device->writeSetting("counter", "true");
            context.device->writeSetting("iq_swap", "false");
            context.device->writeSetting("timetype", "rf_timestamp");

            for (const auto channel : options.channels)
            {
                context.device->setSampleRate(SOAPY_SDR_RX, channel, options.rate);
                context.device->setBandwidth(SOAPY_SDR_RX, channel, options.bandwidth);
                context.device->setFrequency(SOAPY_SDR_RX, channel, options.frequency);
                context.device->setGainMode(SOAPY_SDR_RX, channel, true);
            }

            context.stream =
                context.device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, options.channels);
            context.count =
                context.device->getStreamMTU(context.stream) * static_cast<size_t>(options.blocks);
            double fullScaleValue = 0;
            context.device->getNativeStreamFormat(SOAPY_SDR_RX, 0, fullScaleValue);
            context.fullscale = static_cast<int>(fullScaleValue);

            context.buffers.assign(options.channels.size(),
                                   std::vector<int16_t>(2 * context.count));
            for (auto &buffer : context.buffers) context.bufferPtrs.push_back(buffer.data());

            contexts.push_back(std::move(context));
        }

        for (auto &context : contexts)
        {
            context.device->setHardwareTime(0, "now");
        }

        if (options.startOn1pps)
        {
            std::vector<std::thread> threads;
            std::vector<std::exception_ptr> errors(contexts.size());
            for (size_t index = 0; index < contexts.size(); index++)
            {
                threads.emplace_back([&, index]() {
                    try
                    {
                        contexts[index].device->activateStream(
                            contexts[index].stream, SOAPY_SDR_HAS_TIME);
                    }
                    catch (...)
                    {
                        errors[index] = std::current_exception();
                    }
                });
            }
            for (auto &thread : threads) thread.join();
            for (const auto &error : errors)
            {
                if (error) std::rethrow_exception(error);
            }
        }
        else
        {
            for (auto &context : contexts)
            {
                context.device->activateStream(context.stream);
            }
        }

        for (int readIndex = 0; readIndex < options.reads; readIndex++)
        {
            for (auto &context : contexts)
            {
                int flags = 0;
                long long timeNs = 0;
                const int ret = context.device->readStream(context.stream,
                                                           context.bufferPtrs.data(),
                                                           context.count,
                                                           flags,
                                                           timeNs,
                                                           options.timeoutUs);
                if (ret != static_cast<int>(context.count))
                {
                    throw std::runtime_error("card " + std::to_string(context.card) +
                                             " read returned " + std::to_string(ret) +
                                             ", expected " + std::to_string(context.count));
                }

                bool ok = true;
                for (size_t chanIndex = 0; chanIndex < options.channels.size(); chanIndex++)
                {
                    ok = validateCounter(
                             context.buffers[chanIndex].data(),
                             context.buffers[chanIndex].size(),
                             context.fullscale,
                             "card " + std::to_string(context.card) +
                                 " channel " + std::to_string(options.channels[chanIndex])) &&
                         ok;
                }
                if (!ok) return 1;

                std::cout << "read " << readIndex
                          << ": card=" << context.card
                          << " samples/channel=" << context.count
                          << " channels=";
                for (size_t index = 0; index < options.channels.size(); index++)
                {
                    if (index != 0) std::cout << ",";
                    std::cout << options.channels[index];
                }
                std::cout << " timeNs=" << timeNs << "\n";
            }
        }

        for (auto &context : contexts) cleanup(context);
        return 0;
    }
    catch (const std::exception &ex)
    {
        for (auto &context : contexts) cleanup(context);
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
