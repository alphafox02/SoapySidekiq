/**
 * @file StreamConfiguration.cpp
 * @brief Implements side-effect-free validation of stream setup requests.
 */

#include <SoapySidekiq/StreamConfiguration.hpp>

#include <stdexcept>
#include <algorithm>
#include <utility>
#include <string>

namespace soapy_sidekiq
{

StreamRequest validateStreamRequest(
    const StreamDirection direction,
    const std::string &format,
    const std::vector<std::size_t> &channels,
    const std::size_t available_channels,
    const bool stream_already_configured)
{
    if (stream_already_configured)
    {
        // Report lifecycle misuse before secondary format/channel errors.
        throw std::logic_error(
            direction == StreamDirection::rx
                ? "only one RX stream per device is currently supported"
                : "only one TX stream per device is currently supported");
    }
    if (direction == StreamDirection::tx && channels.size() > 1)
        throw std::invalid_argument("multi-channel TX streams are not currently supported");

    // Soapy's empty channel list convention selects the first logical channel.
    auto selected = channels.empty() ? std::vector<std::size_t>{0} : channels;
    auto unique = selected;
    std::sort(unique.begin(), unique.end());
    if (std::adjacent_find(unique.begin(), unique.end()) != unique.end())
        throw std::invalid_argument("stream channels must be distinct");
    for (const auto channel : selected)
    {
        if (channel >= available_channels)
            throw std::out_of_range(std::string{direction == StreamDirection::rx ? "RX" : "TX"} +
                                    " stream channel " + std::to_string(channel) +
                                    " is unavailable; device has " +
                                    std::to_string(available_channels) + " channel(s)");
    }

    StreamFormat parsed_format;
    if (format == "CS16")
    {
        parsed_format = StreamFormat::cs16;
    }
    else if (format == "CF32")
    {
        parsed_format = StreamFormat::cf32;
    }
    else
    {
        throw std::invalid_argument(
            "unsupported stream format '" + format + "'; expected CS16 or CF32");
    }

    return {direction, parsed_format, selected.front(), std::move(selected)};
}

} // namespace soapy_sidekiq
