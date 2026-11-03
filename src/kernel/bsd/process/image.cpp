/* SPDX-License-Identifier: MPL-2.0 */
#include "kernel/kernel.hpp"
#include "../support.hpp"

namespace ilemu {
void CompatibilityKernel::prepare_process_image(Cpu& cpu, std::string path,
    std::vector<std::string> arguments, std::uint32_t invalid_image_error,
    ProcessImageCommit commit)
{
    auto preparation = process_image_preparer_
        ? process_image_preparer_->prepare(std::move(path),
              working_directory(cpu.processor_id()), std::move(arguments))
        : nullptr;
    if (!preparation) {
        bsd_error(cpu, darwin::error::no_memory);
        return;
    }
    pending_process_images_.insert_or_assign(cpu.processor_id(),
        PendingProcessImage { std::move(preparation), invalid_image_error,
            std::move(commit) });
    process_.waiting_for_events = true;
    bsd_success(cpu, 0);
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
}

bool CompatibilityKernel::deliver_pending_process_image(Cpu& cpu)
{
    const auto found = pending_process_images_.find(cpu.processor_id());
    if (found == pending_process_images_.end() ||
        !found->second.preparation->ready())
        return false;
    auto pending = std::move(found->second);
    pending_process_images_.erase(found);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    if (const auto error = pending.preparation->error()) {
        bsd_error(cpu, error == 2 ? 2 : pending.invalid_image_error);
    } else {
        pending.commit(cpu, pending.preparation->take_image());
    }
    return true;
}
} // namespace ilemu
