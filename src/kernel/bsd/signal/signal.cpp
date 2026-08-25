#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"
#include <cstdint>
#include <vector>

#include "../support.hpp"

namespace ilemu {
namespace {

    bool default_signal_is_ignored(std::uint32_t signal)
    {
        using namespace darwin::signal;
        return signal == urgent || signal == child || signal == io ||
               signal == window_change || signal == information ||
               signal == resume;
    }

    bool default_signal_stops(std::uint32_t signal)
    {
        using namespace darwin::signal;
        return signal == stop || signal == terminal_stop ||
               signal == terminal_input || signal == terminal_output;
    }

} // namespace

std::uint32_t CompatibilityKernel::deliver_signal(std::uint32_t signal)
{
    if (signal == 0 || signal >= darwin::signal::count) {
        return signal == 0 ? 0U : darwin::error::invalid_argument;
    }

    const auto handler = signal_actions_[signal][0];
    const bool unmaskable =
        signal == darwin::signal::kill || signal == darwin::signal::stop;
    if (!unmaskable && handler == darwin::signal::ignore_action) {
        output_.write("[signal] ignored pid=" + std::to_string(process_.pid) +
                      " signal=" + std::to_string(signal) + "\n");
        return 0;
    }
    if (!unmaskable && handler != darwin::signal::default_action) {
        // A complete ARM signal frame/trampoline is a later signal-subsystem
        // milestone. Preserve the accepted delivery without applying the
        // default action; this is also the observable result for a masked
        // pending signal.
        output_.write(
            "[signal] caught-pending pid=" + std::to_string(process_.pid) +
            " signal=" + std::to_string(signal) + "\n");
        return 0;
    }
    if (!unmaskable && (signal_mask_ & (1U << (signal - 1U))) != 0) {
        output_.write(
            "[signal] masked-pending pid=" + std::to_string(process_.pid) +
            " signal=" + std::to_string(signal) + "\n");
        return 0;
    }
    if (default_signal_is_ignored(signal)) {
        return 0;
    }
    if (default_signal_stops(signal)) {
        output_.write(
            "[signal] stop-pending pid=" + std::to_string(process_.pid) +
            " signal=" + std::to_string(signal) + "\n");
        return 0;
    }

    exit_process(0, signal);
    return 0;
}

void CompatibilityKernel::dispatch_bsd_signal(Cpu& cpu, std::uint32_t number)
{
    if (number != darwin::syscall::kill) {
        trace_unknown(cpu, "BSD signal syscall", number);
        bsd_error(cpu, bsd_support::not_implemented);
        return;
    }

    const auto requested_pid = static_cast<std::int32_t>(cpu.registers()[0]);
    const auto signal = cpu.registers()[1];
    if (signal >= darwin::signal::count) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }

    std::vector<std::uint32_t> targets;
    if (requested_pid > 0) {
        targets.push_back(static_cast<std::uint32_t>(requested_pid));
    } else {
        const auto requested_group =
            requested_pid == 0 ? process_.process_group
                               : static_cast<std::uint32_t>(
                                     -static_cast<std::int64_t>(requested_pid));
        for (const auto& [pid, record] : shared_state_->processes) {
            if (record.exited) {
                continue;
            }
            if (requested_pid == -1) {
                if (pid <= 1 || pid == process_.pid) {
                    continue;
                }
            } else if (record.process_group != requested_group) {
                continue;
            }
            targets.push_back(pid);
        }
    }

    bool found = false;
    bool permitted = false;
    for (const auto target_pid : targets) {
        const auto target = shared_state_->processes.find(target_pid);
        if (target == shared_state_->processes.end()) {
            continue;
        }
        found = true;
        if (target->second.exited) {
            permitted = true;
            continue;
        }
        if (process_.effective_uid != 0 &&
            process_.effective_uid != target->second.uid) {
            continue;
        }
        permitted = true;
        if (signal == 0) {
            continue;
        }
        const auto error = signal_delivery_handler_
                               ? signal_delivery_handler_(target_pid, signal)
                           : target_pid == process_.pid
                               ? deliver_signal(signal)
                               : darwin::error::no_such_process;
        if (error != 0) {
            bsd_error(cpu, error);
            return;
        }
    }

    if (!found) {
        bsd_error(cpu, darwin::error::no_such_process);
        return;
    }
    if (!permitted) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return;
    }
    bsd_success(cpu, 0);
    if (process_.exited) {
        cpu.halt(Dynarmic::HaltReason::UserDefined1);
    }
}

} // namespace ilemu
