// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "device_state/pthread_contract.hpp"

#include <stdexcept>

namespace ilemu {
namespace {

    // Fine priority, bsdthread_ctl and QoS default capability bits.
    constexpr std::uint32_t fine_priority_features = 0x2U | 0x4U | 0x40000000U;

    constexpr PthreadContract legacy {
        PthreadRegistrationLayout::Unavailable, 0U,
        PthreadPriorityEncoding::QueueIndex, 0U, 0U, false, false,
    };
    constexpr PthreadContract register_v1 {
        PthreadRegistrationLayout::Arm32RegisterV1, 0U,
        PthreadPriorityEncoding::QueueIndex, 3U, 0U, true, false,
    };
    constexpr PthreadContract register_v1_tsd {
        PthreadRegistrationLayout::Arm32RegisterV1, 0x48U,
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
    constexpr PthreadContract register_v2 {
        PthreadRegistrationLayout::RegisterV2, 0U,
        PthreadPriorityEncoding::QueueIndex, 0U, 0U, true, false,
    };

} // namespace

const PthreadContract& resolve_pthread_contract(DarwinPthreadAbi abi)
{
    switch (abi) {
    case DarwinPthreadAbi::LegacyMachThreads:
        return legacy;
    case DarwinPthreadAbi::BsdThreadRegisterV1:
        return register_v1;
    case DarwinPthreadAbi::BsdThreadRegisterV1TsdBase:
        return register_v1_tsd;
    case DarwinPthreadAbi::BsdThreadRegisterV1TsdBaseFourPriorityWorkqueues:
        return register_v1_tsd_four_queues;
    case DarwinPthreadAbi::BsdThreadRegisterV1ExpandedTsdFourPriorityWorkqueues:
        return register_v1_expanded_tsd_four_queues;
    case DarwinPthreadAbi::BsdThreadRegisterV1ExpandedTsdFinePriorityWorkqueues:
        return register_v1_expanded_tsd_fine_priority;
    case DarwinPthreadAbi::BsdThreadRegisterV2:
        return register_v2;
    }
    throw std::invalid_argument { "Unsupported Darwin pthread ABI" };
}

} // namespace ilemu
