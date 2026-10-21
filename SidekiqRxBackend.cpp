/**
 * @file SidekiqRxBackend.cpp
 * @brief Normalizes Sidekiq SDK receive calls into owned portable results.
 */

#include <SoapySidekiq/SidekiqRxBackend.hpp>

#include <sidekiq_api.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <limits>
#include <vector>

namespace soapy_sidekiq
{

SidekiqRxBackend::SidekiqRxBackend(const std::uint8_t card)
    : card_(card)
{
}

int SidekiqRxBackend::start(const std::uint32_t handle, const bool on_pps)
{
    const auto rx_handle = static_cast<skiq_rx_hdl_t>(handle);
    return on_pps
        ? skiq_start_rx_streaming_on_1pps(card_, rx_handle, 0)
        : skiq_start_rx_streaming(card_, rx_handle);
}

int SidekiqRxBackend::stop(const std::uint32_t handle, const bool on_pps)
{
    const auto rx_handle = static_cast<skiq_rx_hdl_t>(handle);
    return on_pps
        ? skiq_stop_rx_streaming_on_1pps(card_, rx_handle, 0)
        : skiq_stop_rx_streaming(card_, rx_handle);
}

int SidekiqRxBackend::startMulti(const std::vector<std::uint32_t> &handles, const bool on_pps)
{
    if (handles.empty() || handles.size() > std::numeric_limits<std::uint8_t>::max())
        return -EINVAL;
    if (handles.size() == 1)
        return start(handles.front(), on_pps);
    std::vector<skiq_rx_hdl_t> native;
    native.reserve(handles.size());
    for (const auto handle : handles)
        native.push_back(static_cast<skiq_rx_hdl_t>(handle));
    // RX-only APIs: a topology group may contain TX handles as well.
    return on_pps
               ? skiq_start_rx_streaming_multi_on_trigger(card_, native.data(),
                                                          static_cast<std::uint8_t>(native.size()),
                                                          skiq_trigger_src_1pps, 0)
               : skiq_start_rx_streaming_multi_synced(card_, native.data(),
                                                      static_cast<std::uint8_t>(native.size()));
}

int SidekiqRxBackend::stopMulti(const std::vector<std::uint32_t> &handles, const bool on_pps)
{
    if (handles.empty() || handles.size() > std::numeric_limits<std::uint8_t>::max())
        return -EINVAL;
    if (handles.size() == 1)
        return stop(handles.front(), on_pps);
    std::vector<skiq_rx_hdl_t> native;
    native.reserve(handles.size());
    for (const auto handle : handles)
        native.push_back(static_cast<skiq_rx_hdl_t>(handle));
    return on_pps
               ? skiq_stop_rx_streaming_multi_on_trigger(card_, native.data(),
                                                         static_cast<std::uint8_t>(native.size()),
                                                         skiq_trigger_src_1pps, 0)
               : skiq_stop_rx_streaming_multi_immediate(card_, native.data(),
                                                        static_cast<std::uint8_t>(native.size()));
}

RxReceiveResult SidekiqRxBackend::receive()
{
    skiq_rx_hdl_t handle = skiq_rx_hdl_end;
    skiq_rx_block_t *block = nullptr;
    std::uint32_t length = 0;
    const int status = skiq_receive(card_, &handle, &block, &length);
    if (status == skiq_rx_status_no_data)
    {
        return {RxReceiveStatus::no_data, 0, {}, 0, 0, 0};
    }
    if (status == skiq_rx_status_error_overrun)
    {
        return {RxReceiveStatus::overrun, 0, {}, 0, 0, 0};
    }

    // Reject malformed SDK blocks before reading their flexible data array.
    if (status != skiq_rx_status_success || block == nullptr ||
        length < SKIQ_RX_HEADER_SIZE_IN_BYTES ||
        (length - SKIQ_RX_HEADER_SIZE_IN_BYTES) % 4 != 0)
    {
        return {RxReceiveStatus::error, 0, {}, 0, 0,
                status == skiq_rx_status_success ? -EINVAL : status};
    }

    const std::size_t complex_samples =
        (length - SKIQ_RX_HEADER_SIZE_IN_BYTES) / 4;
    std::vector<std::int16_t> samples(complex_samples * 2);
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        samples[index] = block->data[index];
    }
    return {
        RxReceiveStatus::success,
        static_cast<std::uint32_t>(handle),
        std::move(samples),
        block->rf_timestamp,
        block->sys_timestamp,
        0};
}

} // namespace soapy_sidekiq
