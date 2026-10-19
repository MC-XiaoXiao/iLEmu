// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Read and update guest ARM thread register-state flavors.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/mach/thread_act.defs

#include "kernel/kernel.hpp"

#include "kernel/mach_arm_thread_abi.hpp"
#include "mach/mig_wire_abi.hpp"
#include "mach/thread_act_mig_ids.hpp"
#include "mach/xnu_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../support.hpp"
#include "state_server.hpp"

namespace ilemu {
namespace {

    using namespace mach_support;

    constexpr std::uint32_t mach_message_success = 0;
    constexpr std::uint32_t mach_receive_invalid_data = 0x10004008U;
    constexpr std::uint32_t kernel_invalid_argument = 4;
    constexpr std::uint32_t simple_reply_size = 36;
    constexpr std::uint32_t state_reply_prefix_size = 40;

    bool write_words(AddressSpace& memory, std::uint32_t address,
        std::span<const std::uint32_t> words)
    {
        for (std::size_t index = 0; index < words.size(); ++index) {
            if (!memory.write32(address + static_cast<std::uint32_t>(
                                              index * sizeof(std::uint32_t)),
                    words[index])) {
                return false;
            }
        }
        return true;
    }

} // namespace

bool CompatibilityKernel::dispatch_mach_thread_state_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    const auto gets_state =
        request.identifier ==
        mig_message_id(xnu::mig::thread_act::Routine::thread_get_state) ||
        request.identifier ==
        mig_message_id(xnu::mig::thread_act::Routine::act_get_state);
    const auto sets_state =
        request.identifier ==
            mig_message_id(
                xnu::mig::thread_act::Routine::thread_set_state) ||
        request.identifier ==
            mig_message_id(xnu::mig::thread_act::Routine::act_set_state);
    if (!gets_state && !sets_state) {
        return false;
    }

    auto& registers = cpu.registers();
    if (!pending_mach_receives_.empty())
        return false;
    std::lock_guard mach_lock { shared_state_->mach_mutex };
    const auto result = thread_mig::State::try_synchronous_locked(memory_, cpu,
        *shared_state_, process_, registers, request.bits, request.local_port,
        request.address, request.identifier, thread_state_query_, thread_state_update_handler_);
    if (!result)
        return false;
    registers[0] = *result;
    return true;
}

} // namespace ilemu
