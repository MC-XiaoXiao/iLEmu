/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "device_state/pthread_contract.hpp"
#include "kernel/darwin_pthread_runtime.hpp"
#include "mach/xnu_qos_policy.hpp"
#include <cstdint>
#include <optional>

namespace ilemu {
class PthreadPriority {
public:
    [[nodiscard]] static std::uint32_t qos_bucket(
        const PthreadContract& contract, XnuQosTier tier) noexcept
    {
        if (tier == XnuQosTier::Unspecified)
            return 0U;
        const auto index = static_cast<std::uint32_t>(tier);
        const auto maintenance =
            contract.priority_encoding ==
            PthreadPriorityEncoding::FineClassBitsWithMaintenance;
        return 1U << (index + (maintenance ? 7U : 6U));
    }

    struct DispatchWorkqueuePriority {
        std::uint32_t queue { };
        std::uint32_t thread_class { };
        bool overcommit { };
    };

    [[nodiscard]] static std::optional<DispatchWorkqueuePriority>
    decode_dispatch_workqueue_priority(
        const PthreadContract& contract, std::uint32_t encoded) noexcept
    {
        if (contract.priority_encoding == PthreadPriorityEncoding::QueueIndex) {
            return DispatchWorkqueuePriority {
                encoded & ~DarwinPthreadRuntime::workqueue_overcommit,
                encoded & ~DarwinPthreadRuntime::workqueue_overcommit,
                (encoded & DarwinPthreadRuntime::workqueue_overcommit) != 0U,
            };
        }

        // The split-libpthread fine-priority SPI passes a QoS bitfield, not
        // the legacy queue index. Maintenance support adds the lowest class
        // and moves the other class bits up by one.
        constexpr std::uint32_t class_mask = 0x003f'ff00U;
        constexpr std::uint32_t class_shift = 8U;
        constexpr std::uint32_t overcommit_flag = 0x8000'0000U;
        auto class_bit = (encoded & class_mask) >> class_shift;
        if (contract.priority_encoding ==
            PthreadPriorityEncoding::FineClassBitsWithMaintenance) {
            if (class_bit == 0x01U)
                return DispatchWorkqueuePriority { 3U, 0x05U,
                    (encoded & overcommit_flag) != 0U };
            if ((class_bit & 1U) != 0U)
                return std::nullopt;
            class_bit >>= 1U;
        }
        switch (class_bit) {
        case 0x10U:
            return DispatchWorkqueuePriority { 0U, 0x21U,
                (encoded & overcommit_flag) != 0U };
        case 0x08U:
            return DispatchWorkqueuePriority { 0U, 0x19U,
                (encoded & overcommit_flag) != 0U };
        case 0x04U:
            return DispatchWorkqueuePriority { 1U, 0x15U,
                (encoded & overcommit_flag) != 0U };
        case 0x02U:
            return DispatchWorkqueuePriority { 2U, 0x11U,
                (encoded & overcommit_flag) != 0U };
        case 0x01U:
            return DispatchWorkqueuePriority { 3U, 0x09U,
                (encoded & overcommit_flag) != 0U };
        default:
            return std::nullopt;
        }
    }

    [[nodiscard]] static XnuQosTier qos_tier_for_class(
        std::uint32_t thread_class) noexcept
    {
        switch (thread_class) {
        case 0x21U:
            return XnuQosTier::UserInteractive;
        case 0x19U:
            return XnuQosTier::UserInitiated;
        case 0x15U:
            return XnuQosTier::Default;
        case 0x11U:
            return XnuQosTier::Utility;
        case 0x09U:
            return XnuQosTier::Background;
        case 0x05U:
            return XnuQosTier::Maintenance;
        default:
            return XnuQosTier::Unspecified;
        }
    }

    [[nodiscard]] static std::optional<std::int32_t> qos_override_priority(
        const PthreadContract& contract, std::uint32_t encoded) noexcept
    {
        // XNU thread_qos_policy_params: class base priorities. The legacy
        // anonymous override ignores flags/relative priority and donates only
        // QoS. Reuse the selected ABI decoder, never compare encoded integers.
        const auto decoded =
            decode_dispatch_workqueue_priority(contract, encoded);
        if (!decoded)
            return std::nullopt;
        return XnuThreadQosPolicy { qos_tier_for_class(decoded->thread_class) }
            .priority();
    }

    [[nodiscard]] static XnuThreadQosPolicy decode_requested_qos(
        const PthreadContract& contract, std::uint32_t encoded) noexcept
    {
        const auto decoded =
            decode_dispatch_workqueue_priority(contract, encoded);
        if (!decoded)
            return { };
        const auto relative = static_cast<std::int32_t>(
                                  static_cast<std::int8_t>(encoded & 0xffU)) +
                              1;
        return { qos_tier_for_class(decoded->thread_class), relative };
    }
};
} // namespace ilemu
