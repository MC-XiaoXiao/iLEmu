// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Initialize root and child task-port capabilities and identity
// inheritance.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/mach/task.defs

#include "kernel/kernel_mach_task_identity.hpp"

#include "kernel/kernel_shared_state.hpp"

#include <algorithm>
#include <array>
#include <optional>

namespace ilemu::mach_task_identity {
namespace {

    using xnu::ipc::Right;

    constexpr auto send_right = xnu::ipc::type_mask(Right::Send);
    constexpr auto receive_right = xnu::ipc::type_mask(Right::Receive);

    bool install_kernel_send_port(KernelSharedState& state,
        const ProcessContext& process, std::uint32_t name)
    {
        // The fixed early-boot names still denote global kernel ipc_port
        // objects. Materialize them once so fallback MIG dispatch can
        // distinguish a valid kernel destination from an invalid name without
        // hard-coding a routine or firmware version.
        if (!state.mach_port_objects.contains(name)) {
            if (!state.mach_port_objects.create(name))
                return false;
            // Kernel service ports survive a task releasing its last Send
            // right. Later self traps may copy out another right to them.
            ++state.mach_kernel_send_rights[name];
        }
        return state.mach_namespaces.install(
            process.pid, name, name, send_right);
    }

} // namespace

std::uint32_t control_port_locked(
    const KernelSharedState& state, const ProcessContext& process)
{
    const auto named = state.mach_namespaces.resolve(process.pid, process.task_port);
    if (named) {
        const auto task = state.task_port_pids.find(*named);
        if (task != state.task_port_pids.end() && task->second == process.pid)
            return *named;
    }
    // The original name may have been renamed or deallocated.
    const auto task = std::find_if(state.task_port_pids.begin(), state.task_port_pids.end(),
        [&](const auto& entry) { return entry.second == process.pid; });
    return task == state.task_port_pids.end() ? 0U : task->first;
}

std::uint32_t special_port_locked(
    const KernelSharedState& state, std::uint32_t task, std::uint32_t which)
{
    const auto slots = state.task_special_ports.find(task);
    if (slots == state.task_special_ports.end()) return xnu::ipc::null_name;
    const auto port = slots->second.find(which);
    return port == slots->second.end() ? xnu::ipc::null_name : port->second;
}

std::uint32_t name_port_locked(KernelSharedState& state, std::uint32_t pid)
{
    const auto found = std::find_if(state.task_name_port_pids.begin(),
        state.task_name_port_pids.end(),
        [pid](const auto& entry) { return entry.second == pid; });
    if (found != state.task_name_port_pids.end()) return found->first;
    const auto object = state.allocate_mach_object();
    if (!state.mach_port_objects.create(object)) return xnu::ipc::null_name;
    state.task_name_port_pids.emplace(object, pid);
    return object;
}

bool initialize_root(KernelSharedState& state, ProcessContext& process)
{
    process.task_port = initial_task_self_name;
    process.thread_port = initial_thread_self_name;
    process.host_port = initial_host_self_name;
    process.bootstrap_port = initial_bootstrap_name;
    process.clock_port = initial_clock_name;
    process.calendar_clock_port = initial_calendar_clock_name;
    process.io_master_port = initial_io_master_name;
    process.io_registry_options_port = initial_io_registry_options_name;

    const auto task_object = state.allocate_mach_object();
    const auto thread_object = state.allocate_mach_object();
    const auto bootstrap_object = state.allocate_mach_object();

    if (!state.mach_port_objects.create(task_object) ||
        !state.mach_port_objects.create(thread_object) ||
        !state.mach_port_objects.create(bootstrap_object, process.pid)) {
        return false;
    }

    state.mach_namespaces.create_task(process.pid);
    if (!state.mach_namespaces.install(
            process.pid, process.task_port, task_object, send_right) ||
        !state.mach_namespaces.install(
            process.pid, process.thread_port, thread_object, send_right) ||
        !state.mach_namespaces.install(process.pid, process.bootstrap_port,
            bootstrap_object, receive_right)) {
        return false;
    }

    state.task_port_pids.emplace(task_object, process.pid);
    state.task_thread_port_objects[process.pid][0] = thread_object;
    state.task_special_ports[task_object][1] = task_object;
    state.task_special_ports[task_object][2] = initial_host_self_name;
    ++state.mach_kernel_send_rights[task_object];
    ++state.mach_kernel_send_rights[initial_host_self_name];
    state.task_special_ports[task_object][4] = bootstrap_object;
    // The task structure owns a kernel-held reference independent of the
    // bootstrap receive name installed in the root ipc_space.
    ++state.mach_kernel_send_rights[bootstrap_object];
    return install_kernel_send_port(state, process, process.host_port) &&
           install_kernel_send_port(state, process, process.clock_port) &&
           install_kernel_send_port(
               state, process, process.calendar_clock_port) &&
           install_kernel_send_port(state, process, process.io_master_port) &&
           install_kernel_send_port(
               state, process, process.io_registry_options_port);
}

bool inherit_child(KernelSharedState& state, const ProcessContext& parent,
    ProcessContext& child, bool inherit_registered_ports)
{
    const auto parent_task_object = control_port_locked(state, parent);
    if (parent_task_object == xnu::ipc::null_name) {
        return false;
    }

    child.task_port = initial_task_self_name;
    child.thread_port = initial_thread_self_name;
    const auto child_task_object = state.allocate_mach_object();
    const auto child_thread_object = state.allocate_mach_object();
    if (!state.mach_port_objects.create(child_task_object) ||
        !state.mach_port_objects.create(child_thread_object)) {
        return false;
    }

    state.mach_namespaces.create_task(child.pid);
    if (!state.mach_namespaces.install(
            child.pid, child.task_port, child_task_object, send_right) ||
        !state.mach_namespaces.install(
            child.pid, child.thread_port, child_thread_object, send_right)) {
        return false;
    }
    state.task_port_pids[child_task_object] = child.pid;
    state.task_thread_port_objects[child.pid][0] = child_thread_object;

    if (inherit_registered_ports) {
        const auto registered = state.mach_registered_ports.find(parent.pid);
        std::array<std::uint32_t, 3> child_registered { };
        if (registered != state.mach_registered_ports.end()) {
            child_registered = registered->second;
            for (const auto object : child_registered) {
                if (object != xnu::ipc::null_name)
                    ++state.mach_kernel_send_rights[object];
            }
        }
        state.mach_registered_ports[child.pid] = child_registered;
    }

    if (const auto actions =
            state.task_exception_actions.find(parent_task_object);
        actions != state.task_exception_actions.end()) {
        state.task_exception_actions[child_task_object] = actions->second;
        for (const auto& action : actions->second) {
            if (action.port_object != xnu::ipc::null_name)
                ++state.mach_kernel_send_rights[action.port_object];
        }
    }

    state.task_special_ports[child_task_object][1] = child_task_object;
    ++state.mach_kernel_send_rights[child_task_object];
    for (const auto which : { 2U, 4U, 7U, 8U, 9U, 10U }) {
        // Debug control is not inherited. XNU 1228 slot 10 was automount.
        if (which == 10U && state.darwin_abi.task_special_ports !=
                DarwinTaskSpecialPortsAbi::SecurityLedgerAutomount) continue;
        auto object = special_port_locked(state, parent_task_object, which);
        if (object != xnu::ipc::null_name && object != xnu::ipc::dead_name) {
            if (state.mach_port_objects.contains(object))
                ++state.mach_kernel_send_rights[object];
            else
                object = xnu::ipc::dead_name;
        }
        state.task_special_ports[child_task_object][which] = object;
    }
    const auto copy_special = [&](std::uint32_t which, std::uint32_t preferred) {
        const auto object = special_port_locked(state, child_task_object, which);
        if (object == xnu::ipc::null_name || object == xnu::ipc::dead_name) return object;
        return state.mach_namespaces.copyout_at_name(child.pid, object, send_right, preferred)
            .value_or(xnu::ipc::null_name);
    };
    child.host_port = copy_special(2U, initial_host_self_name);
    child.bootstrap_port = copy_special(4U, parent.bootstrap_port);
    for (const auto special :
        { child.clock_port, child.calendar_clock_port,
            child.io_master_port, child.io_registry_options_port }) {
        if (!install_kernel_send_port(state, child, special)) {
            return false;
        }
    }

    // task creation gives the parent a send right named in the parent's own
    // ipc_space. It must not reuse the child's task_self name by convention.
    return state.mach_namespaces
        .copyout(parent.pid, child_task_object, send_right)
        .has_value();
}

} // namespace ilemu::mach_task_identity
