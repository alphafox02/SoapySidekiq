#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>

#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::vector<std::string> parseList(const std::string &text)
{
    std::vector<std::string> values;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        if (!item.empty()) values.push_back(item);
    }
    if (values.empty()) throw std::runtime_error("empty list");
    return values;
}

struct Options
{
    std::vector<std::string> cards = {"0"};
    bool includeTx = false;
    bool expectNonlistedRateReject = false;
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

        if (arg == "--cards") options.cards = parseList(next());
        else if (arg == "--include-tx") options.includeTx = true;
        else if (arg == "--expect-nonlisted-rate-reject")
            options.expectNonlistedRateReject = true;
        else if (arg == "--log-level") options.logLevel = std::stoi(next());
        else throw std::runtime_error("unknown argument " + arg);
    }
    return options;
}

void expectThrows(const std::string &label, const std::function<void()> &action)
{
    try
    {
        action();
    }
    catch (const std::exception &ex)
    {
        std::cout << label << " rejected: " << ex.what() << "\n";
        return;
    }

    throw std::runtime_error(label + " was accepted unexpectedly");
}

void checkRange(const std::string &label, const SoapySDR::RangeList &ranges)
{
    if (ranges.empty())
    {
        throw std::runtime_error(label + " returned no ranges");
    }

    for (const auto &range : ranges)
    {
        if (range.minimum() <= 0.0 || range.maximum() < range.minimum())
        {
            throw std::runtime_error(label + " returned invalid range");
        }
    }
}

void printValues(const std::string &label,
                 const std::vector<double> &values,
                 const size_t maxValues = 40)
{
    std::cout << label << " count=" << values.size() << " values=";
    for (size_t index = 0; index < values.size() && index < maxValues; index++)
    {
        if (index != 0) std::cout << ",";
        std::cout << values[index];
    }
    if (values.size() > maxValues)
    {
        std::cout << ",...";
    }
    std::cout << "\n";
}

void printStrings(const std::string &label,
                  const std::vector<std::string> &values)
{
    std::cout << label << " count=" << values.size() << " values=";
    for (size_t index = 0; index < values.size(); index++)
    {
        if (index != 0) std::cout << ",";
        std::cout << values[index];
    }
    std::cout << "\n";
}

void checkGainRange(const std::string &label, const SoapySDR::Range &range)
{
    if (range.maximum() < range.minimum() || range.step() < 0.0)
    {
        throw std::runtime_error(label + " returned invalid gain range");
    }
}

void printGainInfo(SoapySDR::Device *device,
                   const int direction,
                   const std::string &directionName,
                   const size_t channel)
{
    const std::string prefix = directionName + std::to_string(channel);
    const std::vector<std::string> gains =
        device->listGains(direction, channel);
    printStrings(prefix + " gain_elements", gains);

    const bool hasGainMode = device->hasGainMode(direction, channel);
    std::cout << prefix << " has_gain_mode=" << (hasGainMode ? "true" : "false");
    if (hasGainMode)
    {
        std::cout << " gain_mode="
                  << (device->getGainMode(direction, channel) ? "auto" : "manual");
    }
    std::cout << "\n";

    const SoapySDR::Range aggregateRange =
        device->getGainRange(direction, channel);
    checkGainRange(prefix + " aggregate gain", aggregateRange);
    std::cout << prefix
              << " gain_range=[" << aggregateRange.minimum() << ","
              << aggregateRange.maximum() << "]"
              << " step=" << aggregateRange.step()
              << " value=" << device->getGain(direction, channel)
              << "\n";

    for (const std::string &gain : gains)
    {
        const SoapySDR::Range range =
            device->getGainRange(direction, channel, gain);
        checkGainRange(prefix + " " + gain + " gain", range);
        std::cout << prefix
                  << " gain_" << gain
                  << " range=[" << range.minimum() << ","
                  << range.maximum() << "]"
                  << " step=" << range.step()
                  << " value=" << device->getGain(direction, channel, gain)
                  << "\n";
    }
}

void checkDirection(SoapySDR::Device *device,
                    const int direction,
                    const std::string &directionName,
                    const bool expectNonlistedRateReject)
{
    const size_t channels = device->getNumChannels(direction);
    std::cout << directionName << " channels=" << channels << "\n";

    for (size_t channel = 0; channel < channels; channel++)
    {
        const SoapySDR::RangeList rateRanges =
            device->getSampleRateRange(direction, channel);
        const SoapySDR::RangeList bandwidthRanges =
            device->getBandwidthRange(direction, channel);
        const SoapySDR::RangeList frequencyRanges =
            device->getFrequencyRange(direction, channel);

        checkRange(directionName + std::to_string(channel) + " sample rate",
                   rateRanges);
        checkRange(directionName + std::to_string(channel) + " bandwidth",
                   bandwidthRanges);
        checkRange(directionName + std::to_string(channel) + " frequency",
                   frequencyRanges);

        const SoapySDR::Range rate = rateRanges.front();
        const SoapySDR::Range bandwidth = bandwidthRanges.front();
        const SoapySDR::Range frequency = frequencyRanges.front();
        double fullScale = 0.0;
        const std::string nativeFormat =
            device->getNativeStreamFormat(direction, channel, fullScale);

        std::cout << directionName << channel
                  << " sample_rate=[" << rate.minimum() << ","
                  << rate.maximum() << "]"
                  << " bandwidth=[" << bandwidth.minimum() << ","
                  << bandwidth.maximum() << "]"
                  << " frequency=[" << frequency.minimum() << ","
                  << frequency.maximum() << "]"
                  << " native=" << nativeFormat
                  << " full_scale=" << fullScale << "\n";

        printGainInfo(device, direction, directionName, channel);

        const std::vector<double> rates =
            device->listSampleRates(direction, channel);
        const std::vector<double> bandwidths =
            device->listBandwidths(direction, channel);
        if (rates.empty() || bandwidths.empty())
        {
            throw std::runtime_error(directionName + std::to_string(channel) +
                                     " returned an empty rate/bandwidth list");
        }

        printValues(directionName + std::to_string(channel) + " sample_rates",
                    rates);
        printValues(directionName + std::to_string(channel) + " bandwidths",
                    bandwidths);

        if (expectNonlistedRateReject)
        {
            const double candidate = 12345678.0;
            const bool candidateListed =
                std::find(rates.begin(), rates.end(), candidate) != rates.end();
            if (!candidateListed &&
                candidate >= rate.minimum() &&
                candidate <= rate.maximum())
            {
                expectThrows(directionName + std::to_string(channel) +
                                 " non-listed sample rate",
                             [&]() {
                                 device->setSampleRate(direction,
                                                       channel,
                                                       candidate);
                             });
            }
        }

        expectThrows(directionName + std::to_string(channel) +
                         " sample rate below range",
                     [&]() {
                         device->setSampleRate(direction,
                                               channel,
                                               rate.minimum() / 2.0);
                     });
        expectThrows(directionName + std::to_string(channel) +
                         " sample rate above range",
                     [&]() {
                         device->setSampleRate(direction,
                                               channel,
                                               rate.maximum() + 1000000.0);
                     });
        expectThrows(directionName + std::to_string(channel) +
                         " bandwidth above sample-rate maximum",
                     [&]() {
                         device->setBandwidth(direction,
                                              channel,
                                              rate.maximum() + 1000000.0);
                     });
        expectThrows(directionName + std::to_string(channel) +
                         " frequency below range",
                     [&]() {
                         device->setFrequency(direction,
                                              channel,
                                              frequency.minimum() / 2.0);
                     });
    }
}
}

int main(int argc, char **argv)
{
    try
    {
        const Options options = parseArgs(argc, argv);
        SoapySDR::setLogLevel(static_cast<SoapySDRLogLevel>(options.logLevel));

        for (const std::string &card : options.cards)
        {
            SoapySDR::Kwargs kwargs;
            kwargs["driver"] = "sidekiq";
            kwargs["card"] = card;

            SoapySDR::Device *device = SoapySDR::Device::make(kwargs);
            if (device == nullptr)
            {
                throw std::runtime_error("SoapySDR::Device::make failed for card " + card);
            }

            try
            {
                std::cout << "card=" << card << "\n";
                checkDirection(device,
                               SOAPY_SDR_RX,
                               "RX",
                               options.expectNonlistedRateReject);
                if (options.includeTx)
                {
                    checkDirection(device,
                                   SOAPY_SDR_TX,
                                   "TX",
                                   options.expectNonlistedRateReject);
                }
            }
            catch (...)
            {
                SoapySDR::Device::unmake(device);
                throw;
            }

            SoapySDR::Device::unmake(device);
        }

        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
