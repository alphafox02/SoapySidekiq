/**
 * @file StreamConfiguration.hpp
 * @brief Declares validation and normalization for SoapySDR stream requests.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace soapy_sidekiq
{

/** Direction of a validated stream request. */
enum class StreamDirection
{
    rx,
    tx
};

/** Sample formats implemented by both RX and TX stream paths. */
enum class StreamFormat
{
    cs16,
    cf32
};

/** Normalized stream settings safe to use for hardware configuration. */
struct StreamRequest
{
    /** Requested receive or transmit direction. */
    StreamDirection direction;
    /** Parsed sample format. */
    StreamFormat format;
    /** First logical channel selected for this stream. */
    std::size_t channel;
    /** Selected channels in the caller's buffer order. */
    std::vector<std::size_t> channels{};
};

/**
 * Validate and normalize a Soapy stream request before hardware side effects.
 *
 * An omitted channel selects channel zero. RX accepts multiple distinct channels; TX accepts one
 * channel. One stream per direction is supported.
 *
 * @param direction Requested receive or transmit direction.
 * @param format SoapySDR sample-format name.
 * @param channels Requested logical channel list.
 * @param available_channels Number of channels exposed in this direction.
 * @param stream_already_configured Whether this direction already owns a stream.
 * @return Normalized format, direction, and selected channel.
 * @throws std::logic_error when a stream already exists in this direction.
 * @throws std::invalid_argument for unsupported formats, duplicate RX channels, or multiple TX
 * channels.
 * @throws std::out_of_range when the selected channel is unavailable.
 */
StreamRequest validateStreamRequest(
    StreamDirection direction,
    const std::string &format,
    const std::vector<std::size_t> &channels,
    std::size_t available_channels,
    bool stream_already_configured);

} // namespace soapy_sidekiq
