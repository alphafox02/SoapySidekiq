/**
 * @file SdkCardManager.cpp
 * @brief Implements shared SDK lifetime and per-card acquisition transactions.
 */

#include <SoapySidekiq/SdkCardManager.hpp>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <string>

namespace soapy_sidekiq
{
namespace
{
std::runtime_error sdkError(const char *operation, const std::uint8_t card, const int status)
{
    return std::runtime_error(std::string(operation) + " failed (card " + std::to_string(card) +
                              ", status " + std::to_string(status) + ")");
}
} // namespace

SdkCardManager::SdkCardManager(SdkCardBackend &backend, const std::size_t max_cards)
    : backend_(backend)
{
    if (max_cards == 0 || max_cards > std::numeric_limits<std::uint8_t>::max() + std::size_t{1})
        throw std::invalid_argument("SDK card count must be between 1 and 256");
    cards_.assign(max_cards, CardState::free);
}

void SdkCardManager::acquire(const std::uint8_t card)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (card >= cards_.size())
        throw std::out_of_range("Sidekiq card " + std::to_string(card) + " is out of range");
    if (cards_[card] != CardState::free)
        throw sdkError("card acquisition (already owned or quarantined)", card, EBUSY);
    if (shutdown_failed_)
        throw sdkError("card acquisition after failed SDK shutdown", card, -EIO);
    if (!initialized_)
    {
        const int status = backend_.initialize();
        if (status != 0)
            throw sdkError("skiq_init_without_cards", card, status);
        initialized_ = true;
    }

    std::int64_t owner = 0;
    const int availability = backend_.available(card, owner);
    if (availability != 0)
    {
        // Preflight avoids known SDK failures on unavailable cards; it does not
        // reserve the card. enable() must still handle an intervening claimant.
        const int cleanup = shutdownIfIdle();
        throw std::runtime_error("skiq_is_card_avail failed (card " + std::to_string(card) +
                                 ", owner " + std::to_string(owner) + ", status " +
                                 std::to_string(availability) + ", cleanup status " +
                                 std::to_string(cleanup) + ")");
    }
    const int status = backend_.enable(card);
    if (status != 0)
    {
        const int cleanup = shutdownIfIdle();
        throw std::runtime_error("skiq_enable_cards failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ", cleanup status " +
                                 std::to_string(cleanup) + ")");
    }
    cards_[card] = CardState::owned;
}

int SdkCardManager::release(const std::uint8_t card) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (card >= cards_.size() || cards_[card] == CardState::free)
        return -EINVAL;
    const int status = backend_.disable(card);
    if (status != 0)
    {
        cards_[card] = CardState::quarantined;
        return status;
    }
    cards_[card] = CardState::free;
    return shutdownIfIdle();
}

int SdkCardManager::shutdownIfIdle() noexcept
{
    if (!initialized_ ||
        std::any_of(cards_.begin(), cards_.end(),
                    [](const CardState state) { return state != CardState::free; }))
        return 0;
    const int status = backend_.shutdown();
    shutdown_failed_ = status != 0;
    if (status == 0)
        initialized_ = false;
    return status;
}

} // namespace soapy_sidekiq
