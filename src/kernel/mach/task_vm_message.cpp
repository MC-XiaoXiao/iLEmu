#include "ilemu/bootstrap_mig_ids.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/kernel_clock.hpp"
#include "ilemu/kernel_iokit.hpp"
#include "ilemu/kernel_mach_ipc.hpp"
#include "ilemu/kernel_network.hpp"
#include "ilemu/mach_clock_abi.hpp"
#include "ilemu/mach_host_mig_ids.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_scheduler_abi.hpp"
#include "ilemu/mach_thread_policy_abi.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/task_mig_ids.hpp"
#include "ilemu/thread_act_mig_ids.hpp"
#include "ilemu/vm_map_mig_ids.hpp"
#include "ilemu/xnu_mig_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "support.hpp"

namespace ilemu {

using namespace mach_support;

bool CompatibilityKernel::dispatch_mach_task_vm_message(
    Cpu &cpu, const MachMessageRequest &request) {
  if (dispatch_mach_vm_allocate_message(cpu, request) ||
      dispatch_mach_vm_deallocate_message(cpu, request) ||
      dispatch_mach_vm_protect_message(cpu, request) ||
      dispatch_mach_vm_copy_message(cpu, request) ||
      dispatch_mach_vm_read_message(cpu, request) ||
      dispatch_mach_vm_purgable_message(cpu, request) ||
      dispatch_mach_vm_memory_entry_message(cpu, request) ||
      dispatch_mach_vm_map_message(cpu, request) ||
      dispatch_mach_vm_remap_message(cpu, request) ||
      dispatch_mach_vm_region_message(cpu, request))
    return true;

  auto &registers = cpu.registers();
  const auto message_address = request.address;
  const std::optional<std::uint32_t> bits{request.bits};
  const std::optional<std::uint32_t> remote_port{request.remote_port};
  const std::optional<std::uint32_t> local_port{request.local_port};
  const std::optional<std::uint32_t> message_id{request.identifier};
  const auto creates_suspended_thread =
      *message_id == mig_message_id(xnu792::mig::task::Routine::thread_create);
  const auto creates_running_thread =
      *message_id ==
      mig_message_id(xnu792::mig::task::Routine::thread_create_running);
  if ((creates_suspended_thread || creates_running_thread) &&
      registers[3] >= 40) {
    const auto write_create_error = [&](std::uint32_t result) {
      const std::array<std::uint32_t, 9> reply{
          18,
          36,
          *local_port,
          0,
          0,
          *message_id + 100,
          0,
          1,
          result,
      };
      for (std::size_t index = 0; index < reply.size(); ++index) {
        if (!memory_.write32(message_address +
                                 static_cast<std::uint32_t>(index * 4U),
                             reply[index])) {
          registers[0] = darwin::mach_message::receive_invalid_data;
          return true;
        }
      }
      registers[0] = darwin::mach::success;
      return true;
    };
    const auto &create_arguments =
        xnu792::mig::task::thread_create_running_arguments;
    std::array<std::uint32_t, 16> state{};
    std::uint32_t state_count = 0;
    std::uint32_t guest_cpsr = 0x10U;
    bool valid_state = creates_suspended_thread;
    if (creates_running_thread) {
      const auto flavor =
          memory_.read32(message_address + create_arguments[1].request_offset)
              .value_or(0);
      state_count =
          memory_
              .read32(message_address +
                      create_arguments[2].request_count_offset)
              .value_or(0);
      valid_state = flavor == 1 && state_count >= 17;
      for (std::size_t index = 0; valid_state && index < state.size(); ++index) {
        const auto value = memory_.read32(
            message_address + create_arguments[2].request_offset +
            static_cast<std::uint32_t>(index * sizeof(std::uint32_t)));
        if (!value) {
          valid_state = false;
        } else {
          state[index] = *value;
        }
      }
      guest_cpsr =
          memory_
              .read32(message_address + create_arguments[2].request_offset +
                      16U * sizeof(std::uint32_t))
              .value_or(0);
    }
    if (valid_state && thread_trace_count_ < 16) {
      output_.write("[thread] create-" +
                    std::string(creates_suspended_thread ? "suspended" :
                                                            "running") +
                    " pid=" + std::to_string(process_.pid) + " pc=" +
                    std::to_string(state[15]) + " sp=" +
                    std::to_string(state[13]) + " lr=" +
                    std::to_string(state[14]) + " cpsr=" +
                    std::to_string(guest_cpsr) + " r0=" +
                    std::to_string(state[0]) + " r1=" +
                    std::to_string(state[1]) + " r2=" +
                    std::to_string(state[2]) + " r3=" +
                    std::to_string(state[3]) + " count=" +
                    std::to_string(state_count) + "\n");
      ++thread_trace_count_;
    }
    if (!valid_state) {
      return write_create_error(darwin::mach::invalid_argument);
    }
    const auto processor =
        thread_create_handler_
            ? thread_create_handler_(state, guest_cpsr | 0x10U)
            : std::nullopt;
    if (!processor) {
      return write_create_error(darwin::mach::resource_shortage);
    }
    if (processor) {
      thread_ports_.erase(*processor);
      {
        std::lock_guard mach_lock{shared_state_->mach_mutex};
        if (auto task = shared_state_->task_thread_port_objects.find(
                process_.pid);
            task != shared_state_->task_thread_port_objects.end()) {
          if (const auto old_thread = task->second.find(
                  static_cast<std::uint32_t>(*processor));
              old_thread != task->second.end()) {
            process_.thread_disk_io_policies.erase(old_thread->second);
            task->second.erase(old_thread);
          }
          if (task->second.empty()) {
            shared_state_->task_thread_port_objects.erase(task);
          }
        }
      }
      if (creates_suspended_thread && thread_runnable_handler_ &&
          !thread_runnable_handler_(process_.pid,
                                    static_cast<std::uint32_t>(*processor),
                                    false)) {
        if (thread_terminate_handler_) {
          static_cast<void>(thread_terminate_handler_(process_.pid,
                                                      *processor));
        }
        return write_create_error(darwin::mach::failure);
      }
      std::uint32_t port_object = 0;
      std::uint32_t port_name = 0;
      {
        std::lock_guard mach_lock{shared_state_->mach_mutex};
        port_object = shared_state_->allocate_mach_object();
        static_cast<void>(shared_state_->mach_port_objects.create(port_object));
        shared_state_->mach_queues.try_emplace(port_object);
        port_name =
            shared_state_->mach_namespaces
                .copyout(process_.pid, port_object,
                         xnu792::ipc::type_mask(xnu792::ipc::Right::Send))
                .value_or(0);
        if (port_name != 0) {
          shared_state_->task_thread_port_objects[process_.pid]
                                                 [static_cast<std::uint32_t>(
                                                     *processor)] = port_object;
        }
      }
      if (port_name == 0) {
        std::lock_guard mach_lock{shared_state_->mach_mutex};
        remove_port_object_locked(*shared_state_, port_object);
        if (thread_terminate_handler_) {
          static_cast<void>(thread_terminate_handler_(process_.pid,
                                                      *processor));
        }
        return write_create_error(darwin::mach::resource_shortage);
      }
      thread_ports_[*processor] = port_name;
      const std::array<std::uint32_t, 10> reply{
          0x80000012U,       40, *local_port, 0, 0,
          *message_id + 100, 1,  port_name,   0, 0x00110000U,
      };
      for (std::size_t index = 0; index < reply.size(); ++index) {
        if (!memory_.write32(message_address +
                                 static_cast<std::uint32_t>(index * 4U),
                             reply[index])) {
          registers[0] = 0x10004008U;
          return true;
        }
      }
      registers[0] = 0;
      if (creates_running_thread && scheduler_preemption_query_ &&
          scheduler_preemption_query_(cpu.processor_id())) {
        // Scheduler AST boundary. Unlike an explicit yield, this
        // preserves the remainder of the current first timeslice.
        cpu.request_guest_preemption();
      }
      return true;
    }
  }
  if (*message_id ==
          mig_message_id(xnu792::mig::task::Routine::mach_ports_lookup) &&
      registers[3] >= 52) {
    const std::array<std::uint32_t, 13> reply{
        0x80000012U, 52,          *local_port, 0, 0, *message_id + 100,
        1,           // one OOL ports descriptor
        0,           // empty array address
        0,           // empty array count
        0x02110000U, // OOL_PORTS, MOVE_SEND
        0x00000000U, 0x00000001U,
        0, // init_port_setCnt
    };
    for (std::size_t index = 0; index < reply.size(); ++index) {
      if (!memory_.write32(message_address +
                               static_cast<std::uint32_t>(index * 4U),
                           reply[index])) {
        registers[0] = 0x10004008U;
        return true;
      }
    }
    registers[0] = 0;
    return true;
  }
  if (*message_id ==
          mig_message_id(
              xnu792::mig::mach_port::Routine::mach_port_get_attributes) &&
      registers[3] >= 44) {
    // mach_port_get_attributes.
    const auto &attribute_arguments =
        xnu792::mig::mach_port::mach_port_get_attributes_arguments;
    const auto flavor =
        memory_.read32(message_address + attribute_arguments[2].request_offset)
            .value_or(0);
    const auto requested_count =
        memory_
            .read32(message_address +
                    attribute_arguments[3].request_count_offset)
            .value_or(0);
    if (flavor == 1 && requested_count >= 1) {
      // MACH_PORT_LIMITS_INFO: one natural_t queue limit.
      const auto name =
          memory_
              .read32(message_address + attribute_arguments[1].request_offset)
              .value_or(0);
      std::uint32_t result = 15;
      std::uint32_t queue_limit = xnu792::ipc::default_queue_limit;
      {
        std::lock_guard mach_lock{shared_state_->mach_mutex};
        const auto target =
            target_task_for_port(*shared_state_, process_.pid, *remote_port);
        const auto entry =
            target ? shared_state_->mach_namespaces.lookup(*target, name)
                   : std::nullopt;
        if (entry && (entry->type & xnu792::ipc::type_mask(
                                        xnu792::ipc::Right::Receive)) != 0) {
          result = 0;
          queue_limit = shared_state_->mach_port_objects.lookup(entry->object)
                            .value_or(xnu792::ipc::PortObject{})
                            .queue_limit;
        } else if (entry) {
          result = 17;
        }
      }
      const std::array<std::uint32_t, 11> reply{
          18,          44,          *local_port, 0, 0, *message_id + 100,
          0x00000000U, 0x00000001U, result,
          1, // port_info_outCnt
          queue_limit,
      };
      for (std::size_t index = 0; index < reply.size(); ++index) {
        if (!memory_.write32(message_address +
                                 static_cast<std::uint32_t>(index * 4U),
                             reply[index])) {
          registers[0] = 0x10004008U;
          return true;
        }
      }
      registers[0] = 0;
      return true;
    }
    if (flavor == 2 && requested_count >= 10 && registers[3] >= 80) {
      // MACH_PORT_RECEIVE_STATUS. launchd's demand thread uses the
      // real queue depth to select one member after its zero-length
      // MACH_RCV_LARGE probe reports MACH_RCV_TOO_LARGE.
      const auto name =
          memory_
              .read32(message_address + attribute_arguments[1].request_offset)
              .value_or(0);
      std::uint32_t port_set = 0;
      std::uint32_t mscount = 0;
      std::uint32_t msgcount = 0;
      std::uint32_t result = 15; // KERN_INVALID_NAME
      {
        std::lock_guard mach_lock{shared_state_->mach_mutex};
        const auto target =
            target_task_for_port(*shared_state_, process_.pid, *remote_port);
        const auto entry =
            target ? shared_state_->mach_namespaces.lookup(*target, name)
                   : std::nullopt;
        if (target && entry &&
            (entry->type &
             xnu792::ipc::type_mask(xnu792::ipc::Right::PortSet)) != 0) {
          // A port-set name has a receive right for the set itself, not for
          // an individual queue.  Darwin reports the aggregate pending
          // depth here; GraphicsServices uses it to decide whether its
          // heartbeat may remain paused after a timeout.
          if (const auto set =
                  shared_state_->mach_port_sets.find(entry->object);
              set != shared_state_->mach_port_sets.end()) {
            for (const auto member : set->second) {
              if (const auto queue = shared_state_->mach_queues.find(member);
                  queue != shared_state_->mach_queues.end()) {
                msgcount += static_cast<std::uint32_t>(queue->second.size());
              }
            }
          }
          result = 0;
        } else if (target && entry &&
                   (entry->type &
                    xnu792::ipc::type_mask(xnu792::ipc::Right::Receive)) !=
                       0) {
          for (const auto &[set_name, members] :
               shared_state_->mach_port_sets) {
            if (std::find(members.begin(), members.end(), entry->object) !=
                members.end()) {
              port_set =
                  shared_state_->mach_namespaces.name_for(*target, set_name)
                      .value_or(0);
              break;
            }
          }
          if (const auto object =
                  shared_state_->mach_port_objects.lookup(entry->object)) {
            mscount = object->make_send_count;
          }
          if (const auto queue = shared_state_->mach_queues.find(entry->object);
              queue != shared_state_->mach_queues.end()) {
            msgcount = static_cast<std::uint32_t>(queue->second.size());
          }
          result = 0;
        } else if (entry) {
          result = 17;
        }
      }
      const std::array<std::uint32_t, 20> reply{
          18,          80,          *local_port, 0, 0, *message_id + 100,
          0x00000000U, 0x00000001U, result,
          10,       // port_info_outCnt
          port_set, // mps_pset
          0,        // mps_seqno
          mscount,  // mps_mscount
          5,        // mps_qlimit
          msgcount, // mps_msgcount
          0,        // mps_sorights
          1,        // mps_srights
          0,        // mps_pdrequest
          0,        // mps_nsrequest
          0,        // mps_flags
      };
      if (process_.pid == 1 && port_status_trace_count_ < 64) {
        output_.write("[mach] receive-status name=" + std::to_string(name) +
                      " pset=" + std::to_string(port_set) +
                      " msgcount=" + std::to_string(msgcount) +
                      " result=" + std::to_string(result) + "\n");
        ++port_status_trace_count_;
      }
      for (std::size_t index = 0; index < reply.size(); ++index) {
        if (!memory_.write32(message_address +
                                 static_cast<std::uint32_t>(index * 4U),
                             reply[index])) {
          registers[0] = 0x10004008U;
          return true;
        }
      }
      registers[0] = 0;
      return true;
    }

    const std::array<std::uint32_t, 9> reply{
        18,          36,          *local_port, 0, 0, *message_id + 100,
        0x00000000U, 0x00000001U, 4, // KERN_INVALID_ARGUMENT
    };
    for (std::size_t index = 0; index < reply.size(); ++index) {
      if (!memory_.write32(message_address +
                               static_cast<std::uint32_t>(index * 4U),
                           reply[index])) {
        registers[0] = 0x10004008U;
        return true;
      }
    }
    registers[0] = 0;
    return true;
  }
  if (*message_id == darwin::mach::thread_policy::policy_set_message &&
      registers[3] >= darwin::mach::thread_policy::minimum_request_size) {
    using namespace darwin::mach::thread_policy;
    const auto flavor = memory_.read32(message_address + request_flavor_offset);
    const auto count = memory_.read32(message_address + request_count_offset);
    std::optional<std::size_t> target_thread;
    for (const auto &[processor, port] : thread_ports_) {
      if (port == *remote_port) {
        target_thread = processor;
        break;
      }
    }

    std::vector<std::uint32_t> policy;
    bool valid = flavor && count && target_thread &&
                 *count <= maximum_policy_word_count &&
                 request_policy_offset + static_cast<std::size_t>(*count) *
                                             sizeof(std::uint32_t) <=
                     registers[3];
    if (valid) {
      policy.reserve(*count);
      for (std::uint32_t index = 0; index < *count; ++index) {
        const auto value = memory_.read32(
            message_address +
            static_cast<std::uint32_t>(request_policy_offset +
                                       static_cast<std::size_t>(index) *
                                           sizeof(std::uint32_t)));
        if (!value) {
          valid = false;
          break;
        }
        policy.push_back(*value);
      }
    }
    const auto policy_applied =
        valid && thread_policy_handler_ &&
        thread_policy_handler_(*target_thread, *flavor,
                               std::span<const std::uint32_t>{policy});
    const auto kernel_result =
        policy_applied ? darwin::mach::success : darwin::mach::invalid_argument;
    const std::array<std::uint32_t, simple_reply_word_count> reply{
        18,
        simple_reply_size,
        *local_port,
        0,
        0,
        *message_id + mig_reply_id_delta,
        0,
        1,
        kernel_result,
    };
    for (std::size_t index = 0; index < reply.size(); ++index) {
      if (!memory_.write32(message_address + static_cast<std::uint32_t>(
                                                 index * sizeof(std::uint32_t)),
                           reply[index])) {
        registers[0] = 0x10004008U;
        return true;
      }
    }
    registers[0] = 0;
    if (policy_applied && scheduler_preemption_query_ &&
        scheduler_preemption_query_(cpu.processor_id())) {
      cpu.request_guest_preemption();
    }
    return true;
  }
  return false;
}

} // namespace ilemu
