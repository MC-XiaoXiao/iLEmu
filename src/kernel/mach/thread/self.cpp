#include "ilemu/kernel.hpp"

#include "ilemu/kernel_shared_state.hpp"
#include "ilemu/mach_namespace.hpp"

#include <cstdint>
#include <mutex>

namespace ilemu {

void CompatibilityKernel::dispatch_mach_thread_self_trap(Cpu &cpu) {
  const auto slot = static_cast<std::uint32_t>(cpu.processor_id());
  std::uint32_t name = xnu792::ipc::null_name;
  {
    std::lock_guard mach_lock{shared_state_->mach_mutex};
    const auto task =
        shared_state_->task_thread_port_objects.find(process_.pid);
    if (task != shared_state_->task_thread_port_objects.end()) {
      const auto thread = task->second.find(slot);
      if (thread != task->second.end()) {
        name = shared_state_->mach_namespaces
                   .copyout(process_.pid, thread->second,
                            xnu792::ipc::type_mask(xnu792::ipc::Right::Send))
                   .value_or(xnu792::ipc::null_name);
      }
    }
  }
  if (name != xnu792::ipc::null_name)
    thread_ports_[cpu.processor_id()] = name;
  cpu.registers()[0] = name;
}

} // namespace ilemu
