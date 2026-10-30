// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "device_state/pthread_contract.hpp"

#include <stdexcept>

namespace ilemu {
namespace {

    // Fine priority, bsdthread_ctl and QoS default capability bits.
    constexpr std::uint32_t fine_priority_features = 0x2U | 0x4U | 0x40000000U;
    constexpr std::uint32_t maintenance_feature = 0x10U;

    constexpr PthreadContract legacy {
        PthreadRegistrationLayout::Unavailable, 0U,
        PthreadPriorityEncoding::QueueIndex, 0U, 0U, false, false,
    };
    constexpr PthreadContract register_v1 {
        PthreadRegistrationLayout::Arm32RegisterV1, 0U,
        PthreadPriorityEncoding::QueueIndex, 3U, 0U, true, false,
    };
    constexpr PthreadContract register_v1_tsd_four_queues {
        PthreadRegistrationLayout::Arm32RegisterV1, 0x48U,
        PthreadPriorityEncoding::QueueIndex, 4U, 0U, true, false,
    };
    constexpr PthreadContract register_v1_expanded_tsd_four_queues {
        PthreadRegistrationLayout::Arm32RegisterV1, 0xa4U,
        PthreadPriorityEncoding::QueueIndex, 4U, 0U, true, false,
    };
    constexpr PthreadContract register_v1_expanded_tsd_fine_priority {
        PthreadRegistrationLayout::Arm32RegisterV1, 0xa4U,
        PthreadPriorityEncoding::FineClassBitsV2, 4U, fine_priority_features,
        true, true,
    };
    constexpr PthreadContract register_v1_expanded_tsd_kevent {
        PthreadRegistrationLayout::Arm32RegisterV1, 0xa4U,
        PthreadPriorityEncoding::FineClassBitsWithMaintenance, 4U,
        fine_priority_features | maintenance_feature | 0x01U | 0x40U,
        true, true,
    };

} // namespace

const PthreadContract& resolve_pthread_contract(DarwinVersion version)
{
    switch (version.generation()) {
    case DarwinGeneration::Unknown:
    case DarwinGeneration::IphoneOs1:
    case DarwinGeneration::IphoneOs2:
        return legacy;
    case DarwinGeneration::IphoneOs3:
        return register_v1;
    case DarwinGeneration::Ios4:
    case DarwinGeneration::Ios5:
    case DarwinGeneration::Ios6:
        return register_v1_tsd_four_queues;
    case DarwinGeneration::Ios7:
        return register_v1_expanded_tsd_four_queues;
    case DarwinGeneration::Ios8:
    case DarwinGeneration::Ios9:
        return register_v1_expanded_tsd_fine_priority;
    case DarwinGeneration::Ios10:
        return register_v1_expanded_tsd_kevent;
    }
    throw std::invalid_argument { "Unsupported Darwin pthread ABI" };
}

} // namespace ilemu
