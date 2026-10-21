/**
 * @file SidekiqCardManager.cpp
 * @brief Adapts the module-wide card manager to libsidekiq lifecycle operations.
 */

#include <SoapySidekiq/SdkCardManager.hpp>
#include <SoapySDR/Logger.h>
#include <sidekiq_api.h>

namespace soapy_sidekiq
{
namespace
{
class SidekiqCardBackend final : public SdkCardBackend
{
  public:
    int initialize() noexcept override
    {
        return skiq_init_without_cards();
    }
    int available(const std::uint8_t card, std::int64_t &owner) noexcept override
    {
        pid_t pid = 0;
        const int status = skiq_is_card_avail(card, &pid);
        owner = static_cast<std::int64_t>(pid);
        return status;
    }
    int enable(const std::uint8_t card) noexcept override
    {
        return skiq_enable_cards(&card, 1, skiq_xport_init_level_full);
    }
    int disable(const std::uint8_t card) noexcept override
    {
        return skiq_disable_cards(&card, 1);
    }
    int shutdown() noexcept override
    {
        return skiq_exit();
    }
};

SdkCardManager &cardManager()
{
    // Construct in dependency order. The manager has no implicit SDK shutdown;
    // each device releases its card only after its own workers are stopped.
    static SidekiqCardBackend backend;
    static SdkCardManager manager(backend, SKIQ_MAX_NUM_CARDS);
    return manager;
}
} // namespace

void acquireSidekiqCard(const std::uint8_t card)
{
    cardManager().acquire(card);
}

void releaseSidekiqCard(const std::uint8_t card) noexcept
{
    const int status = cardManager().release(card);
    if (status != 0)
    {
        try
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "SDK card release failed (card %u, status %d)", card,
                          status);
        }
        catch (...)
        {
        } // Cleanup must not throw through constructor rollback.
    }
}

} // namespace soapy_sidekiq
