/**
 * @file SdkCardManager.hpp
 * @brief Coordinates process-wide SDK initialization and exclusive card ownership.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace soapy_sidekiq
{

/** SDK lifecycle boundary. Implementations return status codes and never throw. */
class SdkCardBackend
{
  public:
    virtual ~SdkCardBackend() = default;
    virtual int initialize() noexcept = 0;
    virtual int available(std::uint8_t card, std::int64_t &owner) noexcept = 0;
    virtual int enable(std::uint8_t card) noexcept = 0;
    virtual int disable(std::uint8_t card) noexcept = 0;
    virtual int shutdown() noexcept = 0;
};

/**
 * Serializes SDK lifetime operations for all devices in this module. A card is
 * exclusively owned by one device because its callbacks and stream controls
 * cannot be shared by independent device objects. The backend must outlive this
 * manager. Device owners must release their cards after stopping their workers.
 * This manager cannot coordinate unrelated users of libsidekiq in the same process.
 */
class SdkCardManager
{
  public:
    /** Construct before SDK use; max_cards must fit the SDK's uint8_t card IDs. */
    SdkCardManager(SdkCardBackend &backend, std::size_t max_cards);

    /** Acquire one card, initializing the library only for the first acquisition.
     * Failed initialization never permits shutdown of an SDK owned elsewhere.
     * @throws std::out_of_range for invalid card IDs.
     * @throws std::runtime_error for busy cards and SDK failures, with status details.
     */
    void acquire(std::uint8_t card);

    /**
     * Disable only this card, then exit the SDK if no enabled cards remain.
     * A failed disable quarantines the card until release is retried successfully;
     * it cannot be reacquired, and other devices retain their library lifetime.
     * A failed global shutdown blocks new acquisitions rather than guessing state.
     * @return Backend status, or -EINVAL for an unowned/invalid card.
     */
    int release(std::uint8_t card) noexcept;

  private:
    enum class CardState
    {
        free,
        owned,
        quarantined
    };
    int shutdownIfIdle() noexcept;
    SdkCardBackend &backend_;
    std::vector<CardState> cards_;
    std::mutex mutex_;
    bool initialized_{};
    bool shutdown_failed_{};
};

/** Acquire/release through the module's real SDK backend and singleton manager. */
void acquireSidekiqCard(std::uint8_t card);
void releaseSidekiqCard(std::uint8_t card) noexcept;

} // namespace soapy_sidekiq
