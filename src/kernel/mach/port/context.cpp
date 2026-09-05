#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"
#include "ilemu/mig_wire_abi.hpp"

#include "../support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace ilemu {
namespace {

    // Added after the XNU 792 mach_port subsystem used to generate the legacy
    // adapter table. ARM32 publishes the context as one pointer-width word.
    constexpr std::uint32_t mach_port_get_context_identifier = 3228U;
    constexpr std::uint32_t mach_port_set_context_identifier = 3229U;
    constexpr std::uint32_t get_request_size = 36U;
    constexpr std::uint32_t set_request_size = 40U;
    constexpr std::uint32_t simple_reply_size = 36U;
    constexpr std::uint32_t get_success_reply_size = 40U;
    constexpr std::uint32_t name_offset = 32U;
    constexpr std::uint32_t context_offset = 36U;

} // namespace

bool CompatibilityKernel::dispatch_mach_port_context_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    const auto is_get = request.identifier == mach_port_get_context_identifier;
    if (!is_get && request.identifier != mach_port_set_context_identifier)
        return false;

    auto& registers = cpu.registers();
    const auto required_request_size =
        is_get ? get_request_size : set_request_size;
    const auto required_reply_size =
        is_get ? get_success_reply_size : simple_reply_size;
    if (registers[2] < required_request_size ||
        registers[3] < required_reply_size) {
        registers[0] = darwin::mach_message::receive_invalid_data;
        return true;
    }
    const auto name = memory_.read32(request.address + name_offset);
    const auto supplied_context =
        is_get ? std::optional<std::uint32_t> { 0U }
               : memory_.read32(request.address + context_offset);
    if (!name || !supplied_context) {
        registers[0] = darwin::mach_message::receive_invalid_data;
        return true;
    }

    auto result = darwin::mach::success;
    std::uint32_t returned_context = 0U;
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto target = mach_support::target_task_for_port(
            *shared_state_, process_.pid, request.remote_port);
        const auto entry =
            target ? shared_state_->mach_namespaces.lookup(*target, *name)
                   : std::nullopt;
        if (!target) {
            result = darwin::mach::invalid_task;
        } else if (!entry) {
            result = darwin::mach::invalid_name;
        } else if ((entry->type & xnu792::ipc::type_mask(
                                      xnu792::ipc::Right::Receive)) == 0U) {
            result = darwin::mach::invalid_right;
        } else if (is_get) {
            const auto context =
                shared_state_->mach_port_contexts.find(entry->object);
            if (context != shared_state_->mach_port_contexts.end()) {
                returned_context = static_cast<std::uint32_t>(context->second);
            }
        } else {
            shared_state_->mach_port_contexts[entry->object] =
                *supplied_context;
        }
    }

    const std::array<std::uint32_t,
        get_success_reply_size / sizeof(std::uint32_t)>
        reply {
            darwin::mig_wire::message_bits(
                darwin::mig_wire::disposition_move_send_once),
            is_get && result == darwin::mach::success ? get_success_reply_size
                                                      : simple_reply_size,
            request.local_port,
            0U,
            0U,
            request.identifier + 100U,
            0U,
            1U,
            result,
            returned_context,
        };
    const auto reply_word_count =
        (is_get && result == darwin::mach::success ? get_success_reply_size
                                                   : simple_reply_size) /
        sizeof(std::uint32_t);
    for (std::size_t index = 0; index < reply_word_count; ++index) {
        if (!memory_.write32(
                request.address +
                    static_cast<std::uint32_t>(index * sizeof(std::uint32_t)),
                reply[index])) {
            registers[0] = darwin::mach_message::receive_invalid_data;
            return true;
        }
    }
    registers[0] = darwin::mach::success;
    return true;
}

} // namespace ilemu
