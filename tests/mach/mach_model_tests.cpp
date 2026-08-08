#include <cstddef>
#include <cstdint>
#include <optional>

#include "ilegacysim/mach_namespace.hpp"
#include "ilegacysim/mach_port_object.hpp"
#include "ilegacysim/xnu_scheduler.hpp"
#include "test_support.hpp"

namespace {

using namespace ilegacysim;
using ilegacysim::test::require;

void mach_namespace_test() {
  using namespace ilegacysim::xnu792::ipc;
  MachNamespaceTable namespaces;
  namespaces.create_task(1);
  namespaces.create_task(2);
  const auto first =
      namespaces.allocate(1, 0x90000U, type_mask(Right::Receive));
  const auto second =
      namespaces.allocate(2, 0xa0000U, type_mask(Right::Receive));
  require(first == std::optional<MachName>{first_dynamic_name} &&
              second == std::optional<MachName>{first_dynamic_name},
          "independent XNU ipc_spaces did not reuse a task-local name");
  require(
      namespaces.resolve(1, *first) == std::optional<MachObject>{0x90000U} &&
          namespaces.resolve(2, *second) == std::optional<MachObject>{0xa0000U},
      "task-local Mach names resolved to the wrong port objects");
  const auto send_alias =
      namespaces.copyout(2, 0x90000U, type_mask(Right::Send));
  require(send_alias && *send_alias == first_dynamic_name + name_index_stride &&
              *send_alias != 0x90000U &&
              namespaces.type(2, *send_alias) ==
                  std::optional<MachTypeMask>{type_mask(Right::Send)} &&
              namespaces.type(1, *send_alias) == std::nullopt,
          "ipc_right_copyout reused a global object ID as a local name");
  namespaces.create_task(3);
  const auto restored =
      namespaces.copyout_at_name(3, 0xb0000U, type_mask(Right::Send), 0x503U);
  require(restored == std::optional<MachName>{0x503U} &&
              namespaces.resolve(3, 0x503U) ==
                  std::optional<MachObject>{0xb0000U},
          "explicit bootstrap-name restoration lost object identity");
  require(namespaces.remove_type(1, *first, type_mask(Right::Receive)) &&
              !namespaces.contains(1, *first) &&
              namespaces.contains(2, *second),
          "removing a receive right affected another task namespace");
  require(namespaces.deallocate(2, *send_alias) &&
              !namespaces.contains(2, *send_alias),
          "mach_port_deallocate did not release a task-local send right");
}

void mach_port_object_table_test() {
  using namespace ilegacysim::xnu792::ipc;
  PortObjectTable objects;
  require(!objects.create(0, 1) && !objects.create(0xffff'ffffU, 1),
          "ipc_port table accepted a null/dead object identity");
  require(objects.create(0x90000U, 7) && !objects.create(0x90000U, 9) &&
              objects.size() == 1,
          "ipc_port table did not enforce explicit unique creation");
  require(objects.set_receive_owner(0x90000U, 11) &&
              objects.set_make_send_count(0x90000U, 3) &&
              objects.increment_make_send_count(0x90000U) &&
              objects.sequence_number(0x90000U) ==
                  std::optional<std::uint32_t>{0} &&
              objects.increment_sequence_number(0x90000U) &&
              objects.sequence_number(0x90000U) ==
                  std::optional<std::uint32_t>{1},
          "ipc_port table rejected valid state transitions");
  const auto object = objects.lookup(0x90000U);
  require(object && object->receive_owner == 11 && object->make_send_count == 4,
          "ipc_port table returned incorrect owner/mscount state");
  require(objects.erase(0x90000U) && !objects.set_receive_owner(0x90000U, 12) &&
              !objects.increment_make_send_count(0x90000U) &&
              !objects.increment_sequence_number(0x90000U) &&
              !objects.sequence_number(0x90000U) &&
              !objects.contains(0x90000U) && objects.size() == 0,
          "updating a destroyed ipc_port resurrected its object");
}

void xnu_scheduler_test() {
  using namespace xnu792::scheduler;
  XnuScheduler scheduler{100, 1'000};
  const XnuThreadId normal_a{1, 1};
  const XnuThreadId normal_b{1, 2};
  const XnuThreadId elevated{2, 1};

  require(scheduler.register_thread(normal_a),
          "scheduler rejected its first thread");
  require(scheduler.register_thread(normal_b),
          "scheduler rejected an equal-priority thread");
  require(scheduler.register_thread(elevated, default_base_priority + 1),
          "scheduler rejected an elevated thread");
  require(scheduler.runnable_count() == 3 &&
              scheduler.highest_runnable_priority() ==
                  default_base_priority + 1,
          "scheduler run queue metadata is incorrect");

  auto slice = scheduler.choose_next();
  require(slice && slice->thread == elevated,
          "scheduler did not choose the highest XNU priority");
  require(scheduler.complete_slice(elevated, 100, XnuSliceCompletion::Continue),
          "scheduler could not complete an elevated quantum");

  require(scheduler.set_base_priority(elevated, default_base_priority),
          "scheduler could not lower a base priority");
  slice = scheduler.choose_next();
  require(slice && slice->thread == normal_a,
          "equal-priority queues are not FIFO");
  require(scheduler.complete_slice(normal_a, 40, XnuSliceCompletion::Continue),
          "scheduler could not preserve a partial timeslice");
  slice = scheduler.choose_next();
  require(slice && slice->thread == normal_a && slice->tick_budget == 60,
          "an equal-priority thread preempted the first timeslice");
  require(scheduler.complete_slice(normal_a, 60, XnuSliceCompletion::Continue),
          "scheduler could not expire a timeslice");
  slice = scheduler.choose_next();
  require(slice && slice->thread == normal_b,
          "scheduler did not rotate FIFO order after quantum expiry");
  require(scheduler.complete_slice(normal_b, 1, XnuSliceCompletion::Block),
          "scheduler could not block a running thread");
  require(scheduler.waiting_count() == 1 && scheduler.make_runnable(normal_b),
          "scheduler could not wake a blocked thread");

  require(scheduler.depress(normal_b, 500),
          "scheduler could not depress a runnable thread");
  const auto depressed = scheduler.info(normal_b);
  require(depressed && depressed->depressed &&
              depressed->scheduled_priority == minimum_priority,
          "scheduler did not apply XNU priority depression");

  const auto before = scheduler.info(normal_a);
  require(before && before->scheduling_usage != 0,
          "scheduler did not account contended CPU usage");
  slice = scheduler.choose_next(normal_a);
  require(slice.has_value(),
          "scheduler could not select a GDB-preferred thread");
  require(scheduler.complete_slice(normal_a, 1'000, XnuSliceCompletion::Yield),
          "scheduler could not process an explicit yield");
  const auto after = scheduler.info(normal_a);
  require(after && scheduler.scheduler_tick() != 0 &&
              after->scheduling_usage < before->scheduling_usage + 1'000,
          "scheduler tick did not decay timeshare usage");

  for (;;) {
    const auto selected = scheduler.choose_next();
    require(selected.has_value(), "scheduler lost a runnable thread");
    if (selected->thread == normal_b)
      break;
    require(scheduler.complete_slice(selected->thread, selected->tick_budget,
                                     XnuSliceCompletion::Continue),
            "scheduler could not rotate toward a depressed thread");
  }
  const auto restored = scheduler.info(normal_b);
  require(restored && !restored->depressed &&
              restored->scheduled_priority != minimum_priority,
          "scheduler did not restore depression when the thread resumed");

  XnuScheduler multiprocessor_scheduler{100, 1'000, 2};
  const XnuThreadId global_normal{10, 1};
  const XnuThreadId local_normal{10, 2};
  const XnuThreadId global_high{10, 3};
  const XnuThreadId global_equal{10, 4};
  require(multiprocessor_scheduler.processor_count() == 2 &&
              multiprocessor_scheduler.register_thread(global_normal) &&
              multiprocessor_scheduler.register_thread(local_normal) &&
              multiprocessor_scheduler.bind_thread(local_normal, 1),
          "scheduler could not construct processor-local run queues");
  require(!multiprocessor_scheduler.bind_thread(local_normal, 2),
          "scheduler accepted an out-of-range processor binding");

  const auto processor_zero = multiprocessor_scheduler.choose_next(0);
  const auto processor_one = multiprocessor_scheduler.choose_next(1);
  require(processor_zero && processor_zero->thread == global_normal &&
              processor_zero->processor == 0 && processor_one &&
              processor_one->thread == local_normal &&
              processor_one->processor == 1,
          "two processors did not receive distinct eligible threads");
  require(multiprocessor_scheduler.complete_slice(
              global_normal, 100, XnuSliceCompletion::Block,
              XnuTimeAccounting::Deferred) &&
              multiprocessor_scheduler.complete_slice(
                  local_normal, 100, XnuSliceCompletion::Block,
                  XnuTimeAccounting::Deferred),
          "scheduler could not complete a deferred multi-core batch");
  multiprocessor_scheduler.advance_time(100);
  require(multiprocessor_scheduler.scheduler_tick() == 0,
          "multi-core batch incorrectly summed processor wall time");

  require(multiprocessor_scheduler.make_runnable(local_normal) &&
              multiprocessor_scheduler.register_thread(
                  global_high, default_base_priority + 1),
          "scheduler could not refill local and global queues");
  auto selected = multiprocessor_scheduler.choose_next(1);
  require(selected && selected->thread == global_high,
          "higher global priority did not outrank a local run queue");
  require(multiprocessor_scheduler.complete_slice(global_high, 100,
                                                  XnuSliceCompletion::Block),
          "scheduler could not block a high-priority global thread");

  require(multiprocessor_scheduler.register_thread(global_equal),
          "scheduler could not add a global tie candidate");
  selected = multiprocessor_scheduler.choose_next(1);
  require(selected && selected->thread == local_normal,
          "processor-local queue did not win an equal-priority tie");
  require(multiprocessor_scheduler.complete_slice(local_normal, 100,
                                                  XnuSliceCompletion::Block),
          "scheduler could not block a processor-bound thread");
  selected = multiprocessor_scheduler.choose_next(0);
  require(selected && selected->thread == global_equal,
          "global thread was not available to another processor");
  require(multiprocessor_scheduler.complete_slice(global_equal, 100,
                                                  XnuSliceCompletion::Block),
          "scheduler could not drain its global queue");
  require(multiprocessor_scheduler.make_runnable(local_normal) &&
              !multiprocessor_scheduler.choose_next(0) &&
              multiprocessor_scheduler.choose_next(1).has_value(),
          "processor binding was not enforced during selection");

  XnuScheduler multiquantum_scheduler{100, 1'000, 2};
  const XnuThreadId multiquantum_a{11, 1};
  const XnuThreadId multiquantum_b{11, 2};
  require(multiquantum_scheduler.register_thread(multiquantum_a) &&
              multiquantum_scheduler.register_thread(multiquantum_b),
          "scheduler could not prepare timeshare quanta");
  selected = multiquantum_scheduler.choose_next(0);
  require(selected && selected->thread == multiquantum_a &&
              multiquantum_scheduler.complete_slice(
                  multiquantum_a, selected->tick_budget,
                  XnuSliceCompletion::Continue),
          "scheduler could not complete its first load-adjusted quantum");
  selected = multiquantum_scheduler.choose_next(0);
  require(
      selected && selected->thread == multiquantum_a &&
          multiquantum_scheduler.info(multiquantum_a)->remaining_timeslices ==
              1,
      "underloaded processor set rotated before its XNU timeslice ended");
  require(multiquantum_scheduler.complete_slice(multiquantum_a,
                                                selected->tick_budget,
                                                XnuSliceCompletion::Continue) &&
              multiquantum_scheduler.choose_next(0)->thread == multiquantum_b,
          "equal-priority rotation did not occur after all XNU quanta");

  XnuScheduler realtime_scheduler;
  const XnuThreadId realtime_late{20, 1};
  const XnuThreadId realtime_early{20, 2};
  require(realtime_scheduler.register_thread(realtime_late,
                                             default_base_priority, false) &&
              realtime_scheduler.register_thread(realtime_early,
                                                 default_base_priority, false),
          "scheduler could not register dormant realtime candidates");
  require(!realtime_scheduler.set_realtime(
              realtime_late, 0, minimum_realtime_computation_ticks - 1,
              minimum_realtime_computation_ticks, true),
          "scheduler accepted an undersized realtime computation");
  require(realtime_scheduler.set_realtime(
              realtime_late, 0, minimum_realtime_computation_ticks,
              minimum_realtime_computation_ticks * 3, true) &&
              realtime_scheduler.set_realtime(
                  realtime_early, 0, minimum_realtime_computation_ticks,
                  minimum_realtime_computation_ticks * 2, true) &&
              realtime_scheduler.make_runnable(realtime_late) &&
              realtime_scheduler.make_runnable(realtime_early),
          "scheduler rejected valid XNU realtime constraints");
  selected = realtime_scheduler.choose_next();
  require(selected && selected->thread == realtime_early &&
              selected->tick_budget == minimum_realtime_computation_ticks,
          "realtime run queue was not ordered by earliest deadline");
  const auto realtime_info = realtime_scheduler.info(realtime_early);
  require(realtime_info && realtime_info->realtime &&
              !realtime_info->timeshare &&
              realtime_info->scheduled_priority == realtime_queue_priority,
          "realtime policy metadata does not match XNU 792");

  XnuScheduler candidate_scheduler;
  const XnuThreadId running_realtime{21, 1};
  const XnuThreadId late_realtime{21, 2};
  const XnuThreadId early_realtime{21, 3};
  require(candidate_scheduler.register_thread(
              running_realtime, default_base_priority, false) &&
              candidate_scheduler.set_realtime(
                  running_realtime, 0, minimum_realtime_computation_ticks,
                  minimum_realtime_computation_ticks * 3, true) &&
              candidate_scheduler.make_runnable(running_realtime),
          "scheduler could not prepare the running realtime candidate");
  const auto running_slice = candidate_scheduler.choose_next();
  require(running_slice && running_slice->thread == running_realtime,
          "scheduler could not start the running realtime candidate");
  require(candidate_scheduler.register_thread(
              late_realtime, default_base_priority, false) &&
              candidate_scheduler.register_thread(
                  early_realtime, default_base_priority, false) &&
              candidate_scheduler.set_realtime(
                  late_realtime, 0, minimum_realtime_computation_ticks,
                  minimum_realtime_computation_ticks * 5, true) &&
              candidate_scheduler.set_realtime(
                  early_realtime, 0, minimum_realtime_computation_ticks,
                  minimum_realtime_computation_ticks * 2, true) &&
              candidate_scheduler.make_runnable(late_realtime) &&
              candidate_scheduler.make_runnable(early_realtime),
          "scheduler could not prepare ordered realtime candidates");
  require(candidate_scheduler.preemption_for(running_realtime, 0) ==
              XnuPreemption::Urgent,
          "preemption did not inspect the earliest realtime deadline");
  const auto chosen_candidate = candidate_scheduler.choose_next();
  require(chosen_candidate && chosen_candidate->thread == early_realtime,
          "choose_next did not select the earliest realtime deadline");

  constexpr std::size_t fail_safe_test_quantum_ticks = 100;
  XnuScheduler failsafe_scheduler{fail_safe_test_quantum_ticks, 1'000};
  const XnuThreadId unsafe_realtime{30, 1};
  require(failsafe_scheduler.register_thread(unsafe_realtime,
                                             default_base_priority, false) &&
              failsafe_scheduler.set_realtime(
                  unsafe_realtime, 0, minimum_realtime_computation_ticks,
                  minimum_realtime_computation_ticks * 2, true) &&
              failsafe_scheduler.make_runnable(unsafe_realtime),
          "scheduler could not prepare a realtime fail-safe test");
  constexpr std::size_t unsafe_realtime_quantum_count =
      maximum_unsafe_quanta * fail_safe_test_quantum_ticks /
      minimum_realtime_computation_ticks;
  for (std::size_t quantum = 0; quantum <= unsafe_realtime_quantum_count;
       ++quantum) {
    const auto realtime_slice = failsafe_scheduler.choose_next();
    require(realtime_slice && realtime_slice->thread == unsafe_realtime &&
                failsafe_scheduler.complete_slice(unsafe_realtime,
                                                  realtime_slice->tick_budget,
                                                  XnuSliceCompletion::Continue),
            "scheduler lost an unsafe realtime thread");
  }
  const auto demoted = failsafe_scheduler.info(unsafe_realtime);
  require(demoted && demoted->failsafe && !demoted->realtime &&
              demoted->timeshare &&
              demoted->scheduled_priority == minimum_priority,
          "unsafe realtime computation did not trigger XNU fail-safe demotion");
  failsafe_scheduler.advance_time(failsafe_release_scheduler_ticks * 1'000);
  const auto released = failsafe_scheduler.info(unsafe_realtime);
  require(
      released && !released->failsafe && released->realtime &&
          !released->timeshare &&
          released->scheduled_priority == realtime_queue_priority,
      "XNU fail-safe did not restore realtime policy after its safe period");

  XnuScheduler ast_scheduler{100, 1'000};
  const XnuThreadId ast_current{40, 1};
  const XnuThreadId ast_equal{40, 2};
  const XnuThreadId ast_urgent{40, 3};
  require(ast_scheduler.register_thread(ast_current) &&
              ast_scheduler.choose_next()->thread == ast_current &&
              ast_scheduler.register_thread(ast_equal),
          "scheduler could not prepare an AST preemption test");
  require(ast_scheduler.preemption_for(ast_current, 0) == XnuPreemption::None,
          "equal priority preempted an XNU first timeslice");
  require(ast_scheduler.register_thread(ast_urgent, preempt_priority) &&
              ast_scheduler.set_timeshare(ast_urgent, false) &&
              ast_scheduler.preemption_for(ast_current, 0) ==
                  XnuPreemption::Urgent,
          "fixed high-priority work did not request an urgent XNU AST");
}

void mach_model_suite() {
  mach_namespace_test();
  mach_port_object_table_test();
  xnu_scheduler_test();
}

} // namespace

int main() {
  return ilegacysim::test::run_suite("Mach model", mach_model_suite);
}
