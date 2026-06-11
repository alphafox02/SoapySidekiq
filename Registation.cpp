//  Copyright [2018] <Alexander Hurd>"

#include "SoapySidekiq.hpp"
#include <SoapySDR/Registry.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
struct DiscoveryPart
{
    std::string number;
    std::string revision;
    std::string variant;
};

struct DiscoveryAlias
{
    size_t channel;
    std::string handle;
};

bool matchesDiscoveryFilter(const SoapySDR::Kwargs &args,
                            const SoapySDR::Kwargs &devInfo)
{
    if (args.count("card") != 0 && args.at("card") != devInfo.at("card"))
    {
        return false;
    }

    if (args.count("serial") != 0 && args.at("serial") != devInfo.at("serial"))
    {
        return false;
    }

    const auto requestedChannel = args.count("rx_channel") != 0
        ? args.find("rx_channel")
        : (args.count("channel") != 0 ? args.find("channel") : args.end());

    if (requestedChannel != args.end())
    {
        const auto deviceChannel = devInfo.find("channel");
        if (deviceChannel == devInfo.end() ||
            requestedChannel->second != deviceChannel->second)
        {
            return false;
        }
    }
    else if ((args.count("card") != 0 || args.count("serial") != 0) &&
             devInfo.count("channel") != 0)
    {
        return false;
    }

    return true;
}

const char *partStringOrEmpty(const char *part)
{
    return part == nullptr ? "" : part;
}

std::vector<DiscoveryAlias> discoveryAliasesForPart(const DiscoveryPart &part)
{
    const std::string nv100 = partStringOrEmpty(SKIQ_PART_NUM_STRING_NV100);
    const std::string nvm2 = partStringOrEmpty(SKIQ_PART_NUM_STRING_NVM2);

    if ((!nv100.empty() && part.number == nv100) ||
        (!nvm2.empty() && part.number == nvm2))
    {
        return {
            {0, "A1"},
            {1, "A2"},
            {2, "B1"},
        };
    }

    return {};
}

std::string labelForChannel(const std::string &deviceLabel,
                            const size_t channel,
                            const std::string &handle)
{
    std::string label = deviceLabel + " RX ";
    if (!handle.empty())
    {
        label += handle + " ";
    }
    label += "channel " + std::to_string(channel);
    return label;
}

void addChannelInfo(std::vector<SoapySDR::Kwargs> &devices,
                    const SoapySDR::Kwargs &devInfo,
                    const std::string &deviceLabel,
                    const size_t channel,
                    const std::string &handle)
{
    SoapySDR::Kwargs channelInfo = devInfo;
    channelInfo["channel"] = std::to_string(channel);
    channelInfo["rx_channel"] = std::to_string(channel);
    if (!handle.empty())
    {
        channelInfo["sidekiq_handle"] = handle;
    }
    channelInfo["label"] = labelForChannel(deviceLabel, channel, handle);
    devices.push_back(channelInfo);
}

void addRequestedChannelInfo(std::vector<SoapySDR::Kwargs> &devices,
                             const SoapySDR::Kwargs &args,
                             const SoapySDR::Kwargs &devInfo,
                             const std::string &deviceLabel,
                             const std::vector<DiscoveryAlias> &aliases)
{
    const auto requestedChannel = args.count("rx_channel") != 0
        ? args.find("rx_channel")
        : (args.count("channel") != 0 ? args.find("channel") : args.end());

    if (requestedChannel == args.end())
    {
        return;
    }

    size_t channel = 0;
    try
    {
        channel = std::stoul(requestedChannel->second);
    }
    catch (const std::exception &)
    {
        return;
    }

    std::string handle;
    for (const auto &alias : aliases)
    {
        if (alias.channel == channel)
        {
            handle = alias.handle;
            break;
        }
    }

    addChannelInfo(devices, devInfo, deviceLabel, channel, handle);
}

void addKnownChannelInfos(std::vector<SoapySDR::Kwargs> &devices,
                          const SoapySDR::Kwargs &devInfo,
                          const std::string &deviceLabel,
                          const std::vector<DiscoveryAlias> &aliases)
{
    for (const auto &alias : aliases)
    {
        addChannelInfo(devices,
                       devInfo,
                       deviceLabel,
                       alias.channel,
                       alias.handle);
    }
}

bool readDiscoveryPart(const uint8_t card, DiscoveryPart &part)
{
    std::array<char, SKIQ_PART_NUM_STRLEN> number{};
    std::array<char, SKIQ_REVISION_STRLEN> revision{};
    std::array<char, SKIQ_VARIANT_STRLEN> variant{};

    const int status = skiq_read_part_info(card,
                                           number.data(),
                                           revision.data(),
                                           variant.data());
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_DEBUG,
                      "skiq_read_part_info failed during discovery, card %u, status %d",
                      card, status);
        return false;
    }

    part.number = number.data();
    part.revision = revision.data();
    part.variant = variant.data();
    return true;
}
}

static std::vector<SoapySDR::Kwargs> findSidekiq(const SoapySDR::Kwargs &args)
{
    int                           status = 0;
    std::vector<SoapySDR::Kwargs> results;

    SoapySDR_logf(SOAPY_SDR_TRACE, "findSidekiq");

    uint8_t           number_of_cards = 0;
    uint8_t           card_list[SKIQ_MAX_NUM_CARDS];
    char *            serial_str = nullptr;
    pid_t             card_owner;
    skiq_xport_type_t type = skiq_xport_type_auto;

    SoapySidekiq::sidekiq_devices.clear();

    /* query the list of all Sidekiq cards on the PCIe interface */
    status = skiq_get_cards(type, &number_of_cards, card_list);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Failure: skiq_get_cards, status %d",
                      status);
    }

    for (int i = 0; i < number_of_cards; i++)
    {
        SoapySDR::Kwargs devInfo;
        bool             deviceAvailable = false;
        serial_str = nullptr;

        /* determine the serial number based on the card number */
        status = skiq_read_serial_string(card_list[i], &serial_str);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "Failure: skiq_read_serial_string, status %d",
                          status);
        }

        /* get card availability */
        skiq_is_card_avail(card_list[i], &card_owner);

        deviceAvailable =
            (card_owner == getpid());   // owner must be this process(pid)
                                        
        if (!deviceAvailable)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "Unable to access card #%d, owner pid (%d)", card_list[i],
                          card_owner);
        }

        const std::string serial =
            serial_str == nullptr ? std::string() : std::string(serial_str);
        std::string deviceLabel = "Epiq Solutions Sidekiq card " +
                                  std::to_string(card_list[i]);
        if (!serial.empty())
        {
            deviceLabel += " serial " + serial;
        }

        DiscoveryPart part;
        const bool foundPart = readDiscoveryPart(card_list[i], part);

        devInfo["card"]         = std::to_string(card_list[i]);
        devInfo["label"]        = deviceLabel;
        devInfo["available"]    = deviceAvailable ? "Yes" : "No";
        devInfo["product"]      = "Sidekiq";
        devInfo["serial"]       = serial;
        devInfo["manufacturer"] = "Epiq Solutions";
        if (foundPart)
        {
            devInfo["part_number"] = part.number;
            devInfo["part_revision"] = part.revision;
            devInfo["part_variant"] = part.variant;
        }
        SoapySidekiq::sidekiq_devices.push_back(devInfo);

        const std::vector<DiscoveryAlias> aliases =
            foundPart ? discoveryAliasesForPart(part) : std::vector<DiscoveryAlias>();
        if (args.count("channel") != 0 || args.count("rx_channel") != 0)
        {
            addRequestedChannelInfo(SoapySidekiq::sidekiq_devices,
                                    args,
                                    devInfo,
                                    deviceLabel,
                                    aliases);
        }
        else if (args.count("card") == 0 && args.count("serial") == 0)
        {
            addKnownChannelInfos(SoapySidekiq::sidekiq_devices,
                                 devInfo,
                                 deviceLabel,
                                 aliases);
        }
    }

    //  filtering
    for (const auto &devInfo : SoapySidekiq::sidekiq_devices)
    {
        if (!matchesDiscoveryFilter(args, devInfo))
        {
            continue;
        }

        if (args.count("card") != 0)
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "Found device by card %s",
                          devInfo.at("card").c_str());
        }
        else if (args.count("serial") != 0)
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "Found device by serial %s",
                          args.at("serial").c_str());
        }

        results.push_back(devInfo);
    }

    return results;
}

static SoapySDR::Device *makeSidekiq(const SoapySDR::Kwargs &args)
{
    return new SoapySidekiq(args);
}

static SoapySDR::Registry registerSidekiq("sidekiq", &findSidekiq, &makeSidekiq,
                                          SOAPY_SDR_ABI_VERSION);
