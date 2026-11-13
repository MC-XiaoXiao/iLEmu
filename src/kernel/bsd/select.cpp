/* SPDX-License-Identifier: MPL-2.0 */
// XNU bsd/kern/sys_generic.c: select and pselect share descriptor scanning;
// pselect installs its temporary mask atomically with entering the wait.
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "support.hpp"
#include <limits>

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_select(Cpu& cpu, std::uint32_t number)
{
    auto& registers = cpu.registers();
    const bool pselect = number == 394U;
    std::optional<std::uint64_t> timeout_deadline;
    bool poll_only { };
    if (registers[4] != 0U) {
        const auto seconds_word = memory_.read32(registers[4]);
        const auto fraction_word = memory_.read32(registers[4] + 4U);
        if (!seconds_word || !fraction_word) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
        const auto seconds = static_cast<std::int32_t>(*seconds_word);
        const auto fraction = static_cast<std::int32_t>(*fraction_word);
        const auto fraction_limit = pselect ? 1'000'000'000U : 1'000'000U;
        if (seconds < 0 || fraction < 0 ||
            static_cast<std::uint32_t>(fraction) >= fraction_limit) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        const auto duration = static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL +
            static_cast<std::uint64_t>(fraction) * (pselect ? 1ULL : 1'000ULL);
        poll_only = duration == 0U;
        const auto now = shared_state_->clock.now();
        timeout_deadline = duration > std::numeric_limits<std::uint64_t>::max() - now
            ? std::numeric_limits<std::uint64_t>::max() : now + duration;
    }
    const bool temporary_mask = pselect && registers[5] != 0U;
    if (temporary_mask) {
        const auto mask = memory_.read32(registers[5]);
        if (!mask) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
        signal_state_.suspend(cpu.processor_id(), *mask);
    }
    const auto scan = [&] {
        const auto descriptor_count = registers[0];
        if (descriptor_count > 1024) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return;
        }
        const auto words = (descriptor_count + 31U) / 32U;
        std::uint32_t ready_count = 0;
        std::vector<std::uint32_t> requested_read_words(words);
        std::vector<std::uint32_t> requested_write_words(words);
        for (std::uint32_t word_index = 0; word_index < words; ++word_index) {
            std::uint32_t ready_read_word = 0;
            std::uint32_t ready_write_word = 0;
            if (registers[1] != 0) {
                const auto requested =
                    memory_.read32(registers[1] + word_index * 4U);
                if (!requested) {
                    bsd_error(cpu, bsd_support::bad_address);
                    return;
                }
                requested_read_words[word_index] = *requested;
                for (std::uint32_t bit = 0; bit < 32; ++bit) {
                    const auto fd = word_index * 32U + bit;
                    if (fd >= descriptor_count ||
                        (*requested & (1U << bit)) == 0)
                        continue;
                    if (descriptor_readable(fd)) {
                        ready_read_word |= 1U << bit;
                        ++ready_count;
                    }
                }
                if (!memory_.write32(
                        registers[1] + word_index * 4U, ready_read_word)) {
                    bsd_error(cpu, bsd_support::bad_address);
                    return;
                }
            }
            if (registers[2] != 0) {
                const auto requested =
                    memory_.read32(registers[2] + word_index * 4U);
                if (!requested) {
                    bsd_error(cpu, bsd_support::bad_address);
                    return;
                }
                requested_write_words[word_index] = *requested;
                for (std::uint32_t bit = 0; bit < 32; ++bit) {
                    const auto fd = word_index * 32U + bit;
                    if (fd >= descriptor_count ||
                        (*requested & (1U << bit)) == 0) {
                        continue;
                    }
                    if (descriptor_writable(fd)) {
                        ready_write_word |= 1U << bit;
                        ++ready_count;
                    }
                }
                if (!memory_.write32(
                        registers[2] + word_index * 4U, ready_write_word)) {
                    bsd_error(cpu, bsd_support::bad_address);
                    return;
                }
            }
            if (registers[3] != 0 &&
                !memory_.write32(registers[3] + word_index * 4U, 0)) {
                bsd_error(cpu, bsd_support::bad_address);
                return;
            }
        }
        // Unlike poll(2), select(2) has no per-entry revents field. XNU reports
        // EBADF when any requested descriptor is invalid instead of silently
        // treating it as not ready. Validate both sets before writing filtered
        // results back to the guest so an error cannot leave a partially
        // updated fd_set behind.
        for (std::uint32_t word_index = 0; word_index < words; ++word_index) {
            for (std::uint32_t bit = 0; bit < 32; ++bit) {
                const auto fd = word_index * 32U + bit;
                if (fd >= descriptor_count) {
                    continue;
                }
                const auto requested = ((requested_read_words[word_index] |
                                            requested_write_words[word_index]) &
                                           (1U << bit)) != 0;
                if (requested && !descriptor_valid(fd)) {
                    bsd_error(cpu, bsd_support::bad_file_descriptor);
                    return;
                }
            }
        }
        if (ready_count != 0) {
            bsd_success(cpu, ready_count);
            return;
        }
        if (poll_only) {
            bsd_success(cpu, 0);
            return;
        }
        process_.waiting_for_events = true;
        record_bsd_sleep();
        pending_selects_[cpu.processor_id()] = PendingSelect { descriptor_count,
            registers[1], registers[2], registers[3],
            std::move(requested_read_words), std::move(requested_write_words),
            cpu.processor_id(), timeout_deadline, temporary_mask };
        output_.write(
            "[network] select wait pid=" + std::to_string(process_.pid) +
            " nfds=" + std::to_string(descriptor_count) + "\n");
        bsd_success(cpu, 0);
        cpu.halt(Dynarmic::HaltReason::UserDefined5);
        return;
    };
    scan();
    if (temporary_mask && !pending_selects_.contains(cpu.processor_id()))
        signal_state_.resume(cpu.processor_id());
}

} // namespace ilemu
