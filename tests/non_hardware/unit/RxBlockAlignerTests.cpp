/**
 * @file RxBlockAlignerTests.cpp
 * @brief Checks RF interval alignment, bounded loss, and channel ordering.
 */
#include "TestHarness.hpp"
#include <SoapySidekiq/RxBlockAligner.hpp>
#include <limits>
#include <stdexcept>

using soapy_sidekiq::RxBlockAligner;

TEST_CASE("RX alignment preserves requested channel order despite arrival order")
{
    RxBlockAligner aligner(2, 2);
    aligner.push(1, {11, 12, 13, 14}, 10, 90);
    REQUIRE(!aligner.pop());
    aligner.push(0, {1, 2, 3, 4}, 10, 80);
    const auto block = aligner.pop();
    REQUIRE(block);
    REQUIRE_EQ(block->rf_timestamp, std::uint64_t{10});
    REQUIRE_EQ(block->system_timestamp, std::uint64_t{80});
    REQUIRE_EQ(block->channels[0][0], std::int16_t{1});
    REQUIRE_EQ(block->channels[1][0], std::int16_t{11});
    REQUIRE(!block->discontinuity_before);
    REQUIRE(!aligner.pop());
}

TEST_CASE("RX alignment crops skew without rounding the source system timestamp")
{
    RxBlockAligner aligner(2, 2);
    aligner.push(0, {1, 2, 3, 4}, 10, 3);
    aligner.push(1, {11, 12, 13, 14}, 11, 4);
    auto block = aligner.pop();
    REQUIRE(block);
    REQUIRE_EQ(block->rf_timestamp, std::uint64_t{11});
    REQUIRE_EQ(block->system_timestamp, std::uint64_t{3});
    REQUIRE_EQ(block->system_sample_offset, std::uint64_t{1});
    REQUIRE(block->discontinuity_before);
    REQUIRE((block->channels[0] == std::vector<std::int16_t>{3, 4}));
    REQUIRE((block->channels[1] == std::vector<std::int16_t>{11, 12}));
    aligner.push(0, {5, 6, 7, 8}, 12, 5);
    block = aligner.pop();
    REQUIRE(block);
    REQUIRE_EQ(block->rf_timestamp, std::uint64_t{12});
    REQUIRE(!block->discontinuity_before);
    REQUIRE_EQ(block->channels[1][0], std::int16_t{13});
}

TEST_CASE("RX alignment keeps a complete prefix and marks a missing-channel block")
{
    RxBlockAligner aligner(2, 4);
    for (std::size_t ch = 0; ch < 2; ++ch)
        aligner.push(ch, {1, 2}, 0, 0);
    REQUIRE(!aligner.pop()->discontinuity_before);
    aligner.push(0, {3, 4}, 1, 1);
    aligner.push(0, {5, 6}, 2, 2);
    REQUIRE(aligner.push(1, {15, 16}, 2, 2));
    const auto block = aligner.pop();
    REQUIRE(block);
    REQUIRE_EQ(block->rf_timestamp, std::uint64_t{2});
    REQUIRE(block->discontinuity_before);
    REQUIRE_EQ(block->channels[0][0], std::int16_t{5});
    REQUIRE_EQ(block->channels[1][0], std::int16_t{15});
    REQUIRE(!aligner.pop());
}

TEST_CASE("RX hardware loss marks incoming blocks without discarding a queued prefix")
{
    RxBlockAligner aligner(2, 3);
    for (std::size_t ch = 0; ch < 2; ++ch)
        aligner.push(ch, {1, 2}, 0, 0);
    aligner.markDiscontinuity();
    aligner.markDiscontinuity();
    for (std::size_t ch = 0; ch < 2; ++ch)
        aligner.push(ch, {3, 4}, 1, 1);
    REQUIRE(!aligner.pop()->discontinuity_before);
    REQUIRE(aligner.pop()->discontinuity_before);
    for (std::size_t ch = 0; ch < 2; ++ch)
        aligner.push(ch, {5, 6}, 2, 2);
    REQUIRE(!aligner.pop()->discontinuity_before);
}

TEST_CASE("RX alignment bounds a missing channel and recovers with a marked group")
{
    RxBlockAligner aligner(2, 2);
    for (std::uint64_t timestamp = 0; timestamp < 50; ++timestamp)
        aligner.push(0, {1, 2}, timestamp, timestamp);
    REQUIRE(!aligner.pop());
    aligner.push(1, {11, 12}, 48, 48);
    const auto block = aligner.pop();
    REQUIRE(block);
    REQUIRE_EQ(block->rf_timestamp, std::uint64_t{48});
    REQUIRE(block->discontinuity_before);
    aligner.push(1, {13, 14}, 49, 49);
    REQUIRE(!aligner.pop()->discontinuity_before);
}

TEST_CASE("RX alignment rejects invalid or overlapping intervals without losing valid data")
{
    REQUIRE_THROWS_AS(RxBlockAligner(0, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(RxBlockAligner(1, 0), std::invalid_argument);
    RxBlockAligner aligner(2, 2);
    REQUIRE_THROWS_AS(aligner.push(2, {1, 2}, 0, 0), std::out_of_range);
    REQUIRE_THROWS_AS(aligner.push(0, {}, 0, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(aligner.push(0, {1}, 0, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(aligner.push(0, {1, 2}, std::numeric_limits<std::uint64_t>::max(), 0),
                      std::overflow_error);
    aligner.push(0, {1, 2, 3, 4}, 10, 0);
    REQUIRE_THROWS_AS(aligner.push(0, {5, 6}, 11, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(aligner.push(0, {5, 6}, 9, 0), std::invalid_argument);
    aligner.push(1, {11, 12, 13, 14}, 10, 0);
    REQUIRE_EQ(aligner.pop()->channels[0].size(), std::size_t{4});
}

TEST_CASE("three RX planes with different block sizes yield common contiguous intervals")
{
    RxBlockAligner aligner(3, 3);
    aligner.push(0, {1, 2, 3, 4, 5, 6}, 0, 10);
    aligner.push(1, {11, 12}, 0, 20);
    aligner.push(1, {13, 14, 15, 16}, 1, 21);
    aligner.push(2, {21, 22, 23, 24}, 0, 30);
    aligner.push(2, {25, 26}, 2, 32);
    for (std::uint64_t timestamp = 0; timestamp < 3; ++timestamp)
    {
        const auto block = aligner.pop();
        REQUIRE(block);
        REQUIRE_EQ(block->rf_timestamp, timestamp);
        REQUIRE_EQ(block->system_timestamp, std::uint64_t{10});
        REQUIRE_EQ(block->system_sample_offset, timestamp);
        REQUIRE(!block->discontinuity_before);
        for (const auto &plane : block->channels)
            REQUIRE_EQ(plane.size(), std::size_t{2});
    }
    REQUIRE(!aligner.pop());
}
