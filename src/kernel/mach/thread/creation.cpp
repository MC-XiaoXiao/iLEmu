#include "ilemu/darwin_abi.hpp"
#include "ilemu/kernel.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>

#include "../support.hpp"

namespace ilemu {

using namespace mach_support;

std::optional<CompatibilityKernel::CreatedGuestThread>
CompatibilityKernel::create_guest_thread(
    const std::array<std::uint32_t, 16>& state, std::uint32_t cpsr,
    bool start_suspended, std::uint32_t& kernel_error)
{
    kernel_error = darwin::mach::resource_shortage;
    const auto processor = thread_create_handler_
                               ? thread_create_handler_(state, cpsr | 0x10U)
                               : std::nullopt;
    if (!processor)
        return std::nullopt;

    thread_ports_.erase(*processor);
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        if (auto task =
                shared_state_->task_thread_port_objects.find(process_.pid);
            task != shared_state_->task_thread_port_objects.end()) {
            if (const auto old_thread =
                    task->second.find(static_cast<std::uint32_t>(*processor));
                old_thread != task->second.end()) {
                process_.thread_disk_io_policies.erase(old_thread->second);
                task->second.erase(old_thread);
            }
            if (task->second.empty())
                shared_state_->task_thread_port_objects.erase(task);
        }
    }

    if (start_suspended && thread_runnable_handler_ &&
        !thread_runnable_handler_(
            process_.pid, static_cast<std::uint32_t>(*processor), false)) {
        if (thread_terminate_handler_)
            static_cast<void>(
                thread_terminate_handler_(process_.pid, *processor));
        kernel_error = darwin::mach::failure;
        return std::nullopt;
    }

    std::uint32_t port_object { };
    std::uint32_t port_name { };
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        port_object = shared_state_->allocate_mach_object();
        if (shared_state_->mach_port_objects.create(port_object)) {
            shared_state_->mach_queues.try_emplace(port_object);
            port_name =
                shared_state_->mach_namespaces
                    .copyout(process_.pid, port_object,
                        xnu792::ipc::type_mask(xnu792::ipc::Right::Send))
                    .value_or(0);
            if (port_name != 0) {
                shared_state_->task_thread_port_objects[process_
                        .pid][static_cast<std::uint32_t>(*processor)] =
                    port_object;
            }
        }
    }
    if (port_name == 0) {
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            if (port_object != 0)
                remove_port_object_locked(*shared_state_, port_object);
        }
        if (thread_terminate_handler_)
            static_cast<void>(
                thread_terminate_handler_(process_.pid, *processor));
        return std::nullopt;
    }

    thread_ports_[*processor] = port_name;
    kernel_error = darwin::mach::success;
    return CreatedGuestThread { *processor, port_name };
}

} // namespace ilemu
