#include "ilemu/kernel_mach_task_identity.hpp"

#include "ilemu/kernel_shared_state.hpp"

#include <optional>

namespace ilemu::mach_task_identity {
namespace {

using xnu792::ipc::Right;

constexpr auto send_right = xnu792::ipc::type_mask(Right::Send);
constexpr auto receive_right = xnu792::ipc::type_mask(Right::Receive);

bool install_kernel_send_port(KernelSharedState &state,
                              const ProcessContext &process,
                              std::uint32_t name) {
  return state.mach_namespaces.install(process.pid, name, name, send_right);
}

} // namespace

bool initialize_root(KernelSharedState &state, ProcessContext &process) {
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
  if (!state.mach_namespaces.install(process.pid, process.task_port,
                                     task_object, send_right) ||
      !state.mach_namespaces.install(process.pid, process.thread_port,
                                     thread_object, send_right) ||
      !state.mach_namespaces.install(process.pid, process.bootstrap_port,
                                     bootstrap_object, receive_right)) {
    return false;
  }

  state.task_port_pids.emplace(task_object, process.pid);
  state.task_thread_port_objects[process.pid][0] = thread_object;
  state.task_special_ports[task_object][4] = bootstrap_object;
  // The task structure owns a kernel-held reference independent of the
  // bootstrap receive name installed in the root ipc_space.
  ++state.mach_kernel_send_rights[bootstrap_object];
  return install_kernel_send_port(state, process, process.host_port) &&
         install_kernel_send_port(state, process, process.clock_port) &&
         install_kernel_send_port(state, process,
                                  process.calendar_clock_port) &&
         install_kernel_send_port(state, process, process.io_master_port) &&
         install_kernel_send_port(state, process,
                                  process.io_registry_options_port);
}

bool inherit_child(KernelSharedState &state, const ProcessContext &parent,
                   ProcessContext &child) {
  const auto parent_task_object =
      state.mach_namespaces.resolve(parent.pid, parent.task_port);
  if (!parent_task_object) {
    return false;
  }

  std::optional<std::uint32_t> bootstrap_object;
  if (const auto task = state.task_special_ports.find(*parent_task_object);
      task != state.task_special_ports.end()) {
    if (const auto bootstrap = task->second.find(4);
        bootstrap != task->second.end() && bootstrap->second != 0) {
      bootstrap_object = bootstrap->second;
    }
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
  if (!state.mach_namespaces.install(child.pid, child.task_port,
                                     child_task_object, send_right) ||
      !state.mach_namespaces.install(child.pid, child.thread_port,
                                     child_thread_object, send_right)) {
    return false;
  }
  state.task_port_pids[child_task_object] = child.pid;
  state.task_thread_port_objects[child.pid][0] = child_thread_object;

  if (const auto actions =
          state.task_exception_actions.find(*parent_task_object);
      actions != state.task_exception_actions.end()) {
    state.task_exception_actions[child_task_object] = actions->second;
    for (const auto &action : actions->second) {
      if (action.port_object != xnu792::ipc::null_name)
        ++state.mach_kernel_send_rights[action.port_object];
    }
  }

  if (bootstrap_object) {
    state.task_special_ports[child_task_object][4] = *bootstrap_object;
    ++state.mach_kernel_send_rights[*bootstrap_object];
    child.bootstrap_port =
        state.mach_namespaces
            .copyout_at_name(child.pid, *bootstrap_object, send_right,
                             parent.bootstrap_port)
            .value_or(xnu792::ipc::null_name);
  } else {
    child.bootstrap_port = xnu792::ipc::null_name;
  }

  for (const auto special :
       {child.host_port, child.clock_port, child.calendar_clock_port,
        child.io_master_port,
        child.io_registry_options_port}) {
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
