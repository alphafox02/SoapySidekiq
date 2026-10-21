/**
 * @file SidekiqRxBackend.hpp
 * @brief Declares the Sidekiq SDK adapter used by RxStreamSession.
 */

#pragma once

#include <SoapySidekiq/RxStreamSession.hpp>

#include <cstdint>

namespace soapy_sidekiq
{

/** Adapts the Sidekiq C receive API to the testable RxStreamBackend contract. */
class SidekiqRxBackend final : public RxStreamBackend
{
public:
    /**
     * Bind the backend to one initialized Sidekiq card.
     * @param card SDK card index used by all receive operations.
     */
    explicit SidekiqRxBackend(std::uint8_t card);

    /** @copydoc RxStreamBackend::start */
    int start(std::uint32_t handle, bool on_pps) override;

    /** @copydoc RxStreamBackend::stop */
    int stop(std::uint32_t handle, bool on_pps) override;

    /** @copydoc RxStreamBackend::startMulti */
    int startMulti(const std::vector<std::uint32_t> &handles, bool on_pps) override;

    /** @copydoc RxStreamBackend::stopMulti */
    int stopMulti(const std::vector<std::uint32_t> &handles, bool on_pps) override;

    /** @copydoc RxStreamBackend::receive */
    RxReceiveResult receive() override;

private:
    /** Initialized SDK card index owned by the surrounding device. */
    std::uint8_t card_;
};

} // namespace soapy_sidekiq
