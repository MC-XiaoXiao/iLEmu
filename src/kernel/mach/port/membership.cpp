#include "ilemu/kernel.hpp"

#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_port_object.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "../support.hpp"

namespace ilemu {
namespace {

using namespace mach_support;

constexpr std::uint32_t kernel_success = 0;
constexpr std::uint32_t kernel_already_in_set = 11;
constexpr std::uint32_t kernel_not_in_set = 12;
constexpr std::uint32_t kernel_invalid_name = 15;
constexpr std::uint32_t kernel_invalid_task = 16;
constexpr std::uint32_t kernel_invalid_right = 17;
constexpr std::uint32_t mach_message_success = 0;
constexpr std::uint32_t mach_receive_invalid_data = 0x10004008U;
constexpr std::uint32_t reply_size = 36;

bool is_receive_right(const xnu792::ipc::NameEntry &entry) {
  return (entry.type & xnu792::ipc::type_mask(xnu792::ipc::Right::Receive)) !=
         0;
}

bool is_port_set_right(const xnu792::ipc::NameEntry &entry) {
  return (entry.type & xnu792::ipc::type_mask(xnu792::ipc::Right::PortSet)) !=
         0;
}

} // namespace

bool CompatibilityKernel::dispatch_mach_port_membership_message(
    Cpu &cpu, const MachMessageRequest &request) {
  using Routine = xnu792::mig::mach_port::Routine;
  const auto routine = static_cast<Routine>(request.identifier);
  if (routine != Routine::mach_port_move_member &&
      routine != Routine::mach_port_insert_member &&
      routine != Routine::mach_port_extract_member) {
    return false;
  }

  auto &registers = cpu.registers();
  if (registers[3] < reply_size) {
    return false;
  }
  const auto &arguments =
      routine == Routine::mach_port_move_member
          ? xnu792::mig::mach_port::mach_port_move_member_arguments
      : routine == Routine::mach_port_insert_member
          ? xnu792::mig::mach_port::mach_port_insert_member_arguments
          : xnu792::mig::mach_port::mach_port_extract_member_arguments;
  const auto member =
      memory_.read32(request.address + arguments[1].request_offset).value_or(0);
  const auto set_name =
      memory_.read32(request.address + arguments[2].request_offset).value_or(0);

  std::uint32_t result = kernel_success;
  std::uint32_t target_pid = 0;
  std::uint32_t member_object = 0;
  std::uint32_t set_object = 0;
  std::size_t resulting_member_count = 0;
  {
    std::lock_guard mach_lock{shared_state_->mach_mutex};
    const auto target =
        target_task_for_port(*shared_state_, process_.pid, request.remote_port);
    const auto member_entry =
        target ? shared_state_->mach_namespaces.lookup(*target, member)
               : std::nullopt;
    const auto set_entry =
        target && set_name != xnu792::ipc::null_name
            ? shared_state_->mach_namespaces.lookup(*target, set_name)
            : std::nullopt;
    target_pid = target.value_or(0);
    member_object = member_entry ? member_entry->object : 0;
    set_object = set_entry ? set_entry->object : 0;
    if (!target) {
      result = kernel_invalid_task;
    } else if (!member_entry) {
      result = member == xnu792::ipc::null_name ? kernel_invalid_right
                                                : kernel_invalid_name;
    } else if (!is_receive_right(*member_entry)) {
      result = kernel_invalid_right;
    } else if (routine != Routine::mach_port_move_member ||
               set_name != xnu792::ipc::null_name) {
      if (!set_entry) {
        result = set_name == xnu792::ipc::null_name ? kernel_invalid_right
                                                    : kernel_invalid_name;
      } else if (!is_port_set_right(*set_entry)) {
        result = kernel_invalid_right;
      }
    }
    if (result == kernel_success) {
      if (routine == Routine::mach_port_move_member) {
        bool removed = false;
        for (auto &[candidate_set_object, members] :
             shared_state_->mach_port_sets) {
          static_cast<void>(candidate_set_object);
          const auto previous_size = members.size();
          std::erase(members, member_entry->object);
          removed = removed || members.size() != previous_size;
        }
        if (set_entry) {
          auto &members = shared_state_->mach_port_sets[set_entry->object];
          if (std::find(members.begin(), members.end(), member_entry->object) ==
              members.end()) {
            members.push_back(member_entry->object);
          }
        } else if (!removed) {
          result = kernel_not_in_set;
        }
      } else {
        auto &members = shared_state_->mach_port_sets[set_entry->object];
        const auto existing =
            std::find(members.begin(), members.end(), member_entry->object);
        if (routine == Routine::mach_port_insert_member) {
          // Darwin 8's ipc_port has ip_pset_count and one mqueue link per
          // containing set. KERN_ALREADY_IN_SET applies only to the requested
          // set; the same receive right may belong to another set as well.
          if (existing != members.end()) {
            result = kernel_already_in_set;
          } else {
            members.push_back(member_entry->object);
          }
        } else if (existing == members.end()) {
          result = kernel_not_in_set;
        } else {
          members.erase(existing);
        }
      }
    }
    if (set_object != 0) {
      const auto members = shared_state_->mach_port_sets.find(set_object);
      if (members != shared_state_->mach_port_sets.end())
        resulting_member_count = members->second.size();
    }
  }

  const std::array<std::uint32_t, 9> reply{
      18, reply_size, request.local_port, 0, 0, request.identifier + 100, 0,
      1,  result,
  };
  for (std::size_t index = 0; index < reply.size(); ++index) {
    if (!memory_.write32(request.address +
                             static_cast<std::uint32_t>(index * 4U),
                         reply[index])) {
      registers[0] = mach_receive_invalid_data;
      return true;
    }
  }
  registers[0] = mach_message_success;
  output_.write("[mach] port-membership pid=" + std::to_string(process_.pid) +
                " target=" + std::to_string(target_pid) +
                " id=" + std::to_string(request.identifier) +
                " member=" + std::to_string(member) +
                " member-object=" + std::to_string(member_object) +
                " set=" + std::to_string(set_name) +
                " set-object=" + std::to_string(set_object) +
                " set-members=" + std::to_string(resulting_member_count) +
                " result=" + std::to_string(result) + "\n");
  return true;
}

} // namespace ilemu
