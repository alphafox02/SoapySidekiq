#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>

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
    bool setCurrent = true;
    bool exerciseSwitch = false;
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
        else if (arg == "--no-set-current") options.setCurrent = false;
        else if (arg == "--exercise-switch") options.exerciseSwitch = true;
        else if (arg == "--log-level") options.logLevel = std::stoi(next());
        else throw std::runtime_error("unknown argument " + arg);
    }
    return options;
}

void checkDirection(SoapySDR::Device *device,
                    const int direction,
                    const std::string &directionName,
                    const bool setCurrent,
                    const bool exerciseSwitch)
{
    const size_t channels = device->getNumChannels(direction);
    std::cout << directionName << " channels=" << channels << "\n";

    for (size_t channel = 0; channel < channels; channel++)
    {
        const std::vector<std::string> antennas =
            device->listAntennas(direction, channel);
        const std::string current = device->getAntenna(direction, channel);

        std::cout << directionName << channel << " antennas=";
        for (size_t index = 0; index < antennas.size(); index++)
        {
            if (index != 0) std::cout << ",";
            std::cout << antennas[index];
        }
        std::cout << " current=" << current << "\n";

        if (antennas.empty())
        {
            throw std::runtime_error(directionName + std::to_string(channel) +
                                     " returned no antennas");
        }

        if (setCurrent && current != "NONE")
        {
            device->setAntenna(direction, channel, current);
            const std::string after = device->getAntenna(direction, channel);
            std::cout << directionName << channel
                      << " set-current after=" << after << "\n";
            if (after != current)
            {
                throw std::runtime_error(directionName + std::to_string(channel) +
                                         " set-current changed " + current +
                                         " to " + after);
            }
        }

        if (exerciseSwitch && current != "NONE")
        {
            for (const std::string &antenna : antennas)
            {
                if (antenna == "NONE")
                {
                    continue;
                }

                device->setAntenna(direction, channel, antenna);
                const std::string after = device->getAntenna(direction, channel);
                std::cout << directionName << channel
                          << " set=" << antenna
                          << " after=" << after << "\n";
                if (after != antenna)
                {
                    device->setAntenna(direction, channel, current);
                    throw std::runtime_error(directionName + std::to_string(channel) +
                                             " failed to switch to " + antenna +
                                             "; getAntenna returned " + after);
                }
            }

            device->setAntenna(direction, channel, current);
            const std::string restored = device->getAntenna(direction, channel);
            std::cout << directionName << channel
                      << " restored=" << restored << "\n";
            if (restored != current)
            {
                throw std::runtime_error(directionName + std::to_string(channel) +
                                         " failed to restore " + current +
                                         "; getAntenna returned " + restored);
            }
        }
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
                               options.setCurrent,
                               options.exerciseSwitch);
                if (options.includeTx)
                {
                    checkDirection(device,
                                   SOAPY_SDR_TX,
                                   "TX",
                                   options.setCurrent,
                                   options.exerciseSwitch);
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
