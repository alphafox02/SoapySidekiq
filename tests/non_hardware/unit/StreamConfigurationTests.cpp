/**
 * @file StreamConfigurationTests.cpp
 * @brief Verifies stream format, channel, direction, and ownership validation.
 */

#include "TestHarness.hpp"

#include <SoapySidekiq/StreamConfiguration.hpp>

#include <cstddef>
#include <stdexcept>

using soapy_sidekiq::StreamDirection;
using soapy_sidekiq::StreamFormat;
using soapy_sidekiq::validateStreamRequest;

// Validation tests deliberately avoid constructing a Sidekiq device or buffers.
TEST_CASE("an omitted stream channel selects channel zero")
{
    const auto rx = validateStreamRequest(
        StreamDirection::rx, "CS16", {}, 2, false);
    REQUIRE(rx.direction == StreamDirection::rx);
    REQUIRE(rx.format == StreamFormat::cs16);
    REQUIRE_EQ(rx.channel, std::size_t{0});

    const auto tx = validateStreamRequest(
        StreamDirection::tx, "CF32", {}, 1, false);
    REQUIRE(tx.direction == StreamDirection::tx);
    REQUIRE(tx.format == StreamFormat::cf32);
    REQUIRE_EQ(tx.channel, std::size_t{0});
}

TEST_CASE("stream validation preserves an explicitly selected channel")
{
    const auto request = validateStreamRequest(
        StreamDirection::rx, "CF32", {2}, 3, false);
    REQUIRE_EQ(request.channel, std::size_t{2});
    REQUIRE(request.format == StreamFormat::cf32);
}

TEST_CASE("RX and TX accept every format implemented by the driver")
{
    for (const auto direction : {StreamDirection::rx, StreamDirection::tx})
    {
        REQUIRE(validateStreamRequest(direction, "CS16", {}, 1, false).format ==
                StreamFormat::cs16);
        REQUIRE(validateStreamRequest(direction, "CF32", {}, 1, false).format ==
                StreamFormat::cf32);
    }
}

TEST_CASE("stream validation rejects unknown and case-mismatched formats")
{
    REQUIRE_THROWS_AS(
        validateStreamRequest(StreamDirection::rx, "", {}, 1, false),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        validateStreamRequest(StreamDirection::rx, "cs16", {}, 1, false),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        validateStreamRequest(StreamDirection::tx, "CU8", {}, 1, false),
        std::invalid_argument);
}

TEST_CASE("stream validation rejects duplicate RX channels and multiple TX channels")
{
    REQUIRE_THROWS_AS(validateStreamRequest(StreamDirection::rx, "CS16", {0, 0}, 2, false),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(
        validateStreamRequest(StreamDirection::tx, "CS16", {0, 1}, 2, false),
        std::invalid_argument);
}

TEST_CASE("stream validation rejects channels not exposed by the device")
{
    REQUIRE_THROWS_AS(
        validateStreamRequest(StreamDirection::rx, "CS16", {2}, 2, false),
        std::out_of_range);
    REQUIRE_THROWS_AS(
        validateStreamRequest(StreamDirection::tx, "CS16", {}, 0, false),
        std::out_of_range);
}

TEST_CASE("stream validation rejects a second stream in the same direction")
{
    REQUIRE_THROWS_AS(
        validateStreamRequest(StreamDirection::rx, "CS16", {}, 1, true),
        std::logic_error);
    REQUIRE_THROWS_AS(
        validateStreamRequest(StreamDirection::tx, "CS16", {}, 1, true),
        std::logic_error);
}

TEST_CASE("an active stream is rejected before other request validation")
{
    REQUIRE_THROWS_AS(
        validateStreamRequest(StreamDirection::rx, "bad", {10, 11}, 0, true),
        std::logic_error);
}

TEST_CASE("multi RX requests preserve buffer order and validate every channel")
{
    const auto request = validateStreamRequest(StreamDirection::rx, "CS16", {2, 0}, 3, false);
    REQUIRE((request.channels == std::vector<std::size_t>{2, 0}));
    REQUIRE_EQ(request.channel, std::size_t{2});
    REQUIRE_THROWS_AS(validateStreamRequest(StreamDirection::rx, "CF32", {0, 3}, 3, false),
                      std::out_of_range);
}
