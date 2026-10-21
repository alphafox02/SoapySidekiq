/**
 * @file SdkCardManagerTests.cpp
 * @brief Checks shared library lifetime, card isolation, and acquisition rollback.
 */

#include "TestHarness.hpp"
#include <SoapySidekiq/SdkCardManager.hpp>
#include <SoapySidekiq/ScopeExit.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct FakeSdk final : soapy_sidekiq::SdkCardBackend
{
    int init_status{}, available_status{}, enable_status{}, disable_status{}, exit_status{};
    unsigned initializes{}, exits{}, availability_checks{}, enables{}, disables{};
    std::array<bool, 4> enabled{};
    bool initialized{};
    bool invalid_lifetime{};

    int initialize() noexcept override
    {
        ++initializes;
        if (initialized)
            invalid_lifetime = true;
        if (init_status == 0)
            initialized = true;
        return init_status;
    }
    int available(std::uint8_t, std::int64_t &owner) noexcept override
    {
        ++availability_checks;
        if (!initialized)
            invalid_lifetime = true;
        owner = 1234;
        return available_status;
    }
    int enable(const std::uint8_t card) noexcept override
    {
        ++enables;
        if (!initialized || enabled[card])
            invalid_lifetime = true;
        if (enable_status == 0)
            enabled[card] = true;
        return enable_status;
    }
    int disable(const std::uint8_t card) noexcept override
    {
        ++disables;
        if (!initialized || !enabled[card])
            invalid_lifetime = true;
        if (disable_status == 0)
            enabled[card] = false;
        return disable_status;
    }
    int shutdown() noexcept override
    {
        ++exits;
        if (!initialized)
            invalid_lifetime = true;
        for (const bool active : enabled)
            if (active)
                invalid_lifetime = true;
        if (exit_status == 0)
            initialized = false;
        return exit_status;
    }
};
} // namespace

TEST_CASE("closing one card preserves another card in the same process")
{
    FakeSdk sdk;
    soapy_sidekiq::SdkCardManager manager(sdk, 4);
    manager.acquire(0);
    manager.acquire(2);
    REQUIRE_EQ(sdk.initializes, 1u);
    REQUIRE_EQ(manager.release(0), 0);
    REQUIRE(sdk.initialized && sdk.enabled[2] && !sdk.enabled[0]);
    REQUIRE_EQ(sdk.exits, 0u);
    REQUIRE_EQ(manager.release(2), 0);
    REQUIRE_EQ(sdk.exits, 1u);
    REQUIRE(!sdk.initialized && !sdk.invalid_lifetime);
}

TEST_CASE("a failed second card acquisition does not shut down a live first card")
{
    for (const bool busy : {false, true})
    {
        FakeSdk sdk;
        soapy_sidekiq::SdkCardManager manager(sdk, 4);
        manager.acquire(0);
        if (busy)
            sdk.available_status = EBUSY;
        else
            sdk.enable_status = -EIO;
        REQUIRE_THROWS_AS(manager.acquire(1), std::runtime_error);
        REQUIRE(sdk.initialized && sdk.enabled[0]);
        REQUIRE_EQ(sdk.exits, 0u);
        REQUIRE_EQ(sdk.enables, busy ? 1u : 2u);
        REQUIRE_EQ(manager.release(0), 0);
        REQUIRE(!sdk.invalid_lifetime);
    }
}

TEST_CASE("constructor rollback releases only its newly acquired card")
{
    FakeSdk sdk;
    soapy_sidekiq::SdkCardManager manager(sdk, 4);
    manager.acquire(0);
    try
    {
        manager.acquire(1);
        soapy_sidekiq::ScopeExit rollback([&]() noexcept { (void)manager.release(1); });
        throw std::runtime_error("parameter query failed");
    }
    catch (const std::runtime_error &)
    {
    }
    REQUIRE(sdk.enabled[0] && !sdk.enabled[1]);
    REQUIRE_EQ(sdk.exits, 0u);
    REQUIRE_EQ(manager.release(0), 0);
    REQUIRE(!sdk.invalid_lifetime);
}

TEST_CASE("busy or absent cards never reach enable and roll back first initialization")
{
    for (const int status : {EBUSY, -ENODEV, -ERANGE})
    {
        FakeSdk sdk;
        sdk.available_status = status;
        soapy_sidekiq::SdkCardManager manager(sdk, 4);
        try
        {
            manager.acquire(1);
            REQUIRE(false);
        }
        catch (const std::runtime_error &error)
        {
            const std::string message(error.what());
            REQUIRE(message.find("card 1") != std::string::npos);
            REQUIRE(message.find("owner 1234") != std::string::npos);
            REQUIRE(message.find("status " + std::to_string(status)) != std::string::npos);
        }
        REQUIRE_EQ(sdk.enables, 0u);
        REQUIRE_EQ(sdk.exits, 1u);
        REQUIRE(!sdk.invalid_lifetime);
    }
}

TEST_CASE("failed initialization never shuts down an SDK potentially owned elsewhere")
{
    FakeSdk sdk;
    sdk.init_status = -EEXIST;
    soapy_sidekiq::SdkCardManager manager(sdk, 4);
    REQUIRE_THROWS_AS(manager.acquire(0), std::runtime_error);
    REQUIRE_EQ(sdk.exits, 0u);
    REQUIRE_EQ(sdk.availability_checks, 0u);
    REQUIRE_EQ(manager.release(0), -EINVAL);
}

TEST_CASE("failed first enable exits once and allows a later fresh acquisition")
{
    FakeSdk sdk;
    sdk.enable_status = -EIO;
    soapy_sidekiq::SdkCardManager manager(sdk, 4);
    REQUIRE_THROWS_AS(manager.acquire(0), std::runtime_error);
    REQUIRE_EQ(sdk.exits, 1u);
    sdk.enable_status = 0;
    manager.acquire(0);
    REQUIRE_EQ(sdk.initializes, 2u);
    REQUIRE_EQ(manager.release(0), 0);
    REQUIRE_EQ(sdk.exits, 2u);
    REQUIRE(!sdk.invalid_lifetime);
}

TEST_CASE("duplicate owners and invalid releases cannot replace callbacks or shorten lifetime")
{
    FakeSdk sdk;
    soapy_sidekiq::SdkCardManager manager(sdk, 4);
    manager.acquire(1);
    REQUIRE_THROWS_AS(manager.acquire(1), std::runtime_error);
    REQUIRE_THROWS_AS(manager.acquire(4), std::out_of_range);
    REQUIRE_EQ(manager.release(0), -EINVAL);
    REQUIRE_EQ(manager.release(255), -EINVAL);
    REQUIRE_EQ(sdk.enables, 1u);
    REQUIRE_EQ(sdk.disables, 0u);
    REQUIRE_EQ(manager.release(1), 0);
    REQUIRE_EQ(manager.release(1), -EINVAL);
    REQUIRE_EQ(sdk.exits, 1u);
}

TEST_CASE("failed card disable retains library state until a successful retry")
{
    FakeSdk sdk;
    soapy_sidekiq::SdkCardManager manager(sdk, 4);
    manager.acquire(0);
    manager.acquire(1);
    sdk.disable_status = -EIO;
    REQUIRE_EQ(manager.release(0), -EIO);
    REQUIRE_THROWS_AS(manager.acquire(0), std::runtime_error);
    sdk.disable_status = 0;
    REQUIRE_EQ(manager.release(1), 0);
    REQUIRE_EQ(sdk.exits, 0u);
    REQUIRE(sdk.enabled[0]);
    REQUIRE_EQ(manager.release(0), 0);
    REQUIRE_EQ(sdk.exits, 1u);
    REQUIRE(!sdk.invalid_lifetime);
}

TEST_CASE("failed SDK shutdown blocks new acquisitions and preserves cleanup diagnostics")
{
    for (const bool rollback : {false, true})
    {
        FakeSdk sdk;
        sdk.exit_status = -EIO;
        soapy_sidekiq::SdkCardManager manager(sdk, 4);
        if (rollback)
        {
            sdk.available_status = EBUSY;
            REQUIRE_THROWS_AS(manager.acquire(0), std::runtime_error);
        }
        else
        {
            manager.acquire(0);
            REQUIRE_EQ(manager.release(0), -EIO);
        }
        REQUIRE_THROWS_AS(manager.acquire(1), std::runtime_error);
        REQUIRE_EQ(sdk.initializes, 1u);
        REQUIRE_EQ(sdk.exits, 1u);
        REQUIRE(!sdk.invalid_lifetime);
    }
}

TEST_CASE("concurrent acquisitions of one card yield exactly one exclusive owner")
{
    FakeSdk sdk;
    soapy_sidekiq::SdkCardManager manager(sdk, 4);
    std::atomic<unsigned> successes{}, failures{};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i)
        threads.emplace_back(
            [&]()
            {
                try
                {
                    manager.acquire(0);
                    ++successes;
                }
                catch (const std::runtime_error &)
                {
                    ++failures;
                }
            });
    for (auto &thread : threads)
        thread.join();
    REQUIRE_EQ(successes.load(), 1u);
    REQUIRE_EQ(failures.load(), 7u);
    REQUIRE_EQ(sdk.initializes, 1u);
    REQUIRE_EQ(sdk.enables, 1u);
    REQUIRE_EQ(manager.release(0), 0);
    REQUIRE(!sdk.invalid_lifetime);
}

TEST_CASE("invalid manager bounds are rejected before SDK initialization")
{
    FakeSdk sdk;
    REQUIRE_THROWS_AS(soapy_sidekiq::SdkCardManager(sdk, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(soapy_sidekiq::SdkCardManager(sdk, 257), std::invalid_argument);
    REQUIRE_EQ(sdk.initializes, 0u);
}
