/* SPDX-License-Identifier: MPL-2.0 */
#include "../mach/support.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mach_thread_policy_abi.hpp"
#include "pthread_priority.hpp"
#include "support.hpp"
#include <algorithm>
#include <array>

namespace ilemu {

bool CompatibilityKernel::register_bsd_pthread(Cpu& cpu)
{
    const auto& contract = shared_state_->pthread_contract;
    auto& registers = cpu.registers();
    DarwinPthreadRegistration registration { registers[0], registers[1],
        registers[2], registers[3], registers[4], registers[5],
        contract.thread_pointer_offset };
    if (pthread_runtime_.registration() ||
        registration.pthread_size >
            DarwinPthreadRuntime::maximum_pthread_size) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return true;
    }
    if (contract.registration_data_size != 0U && registers[3] != 0U) {
        std::array<std::byte, 28> data { };
        const auto size =
            std::min(registers[4], contract.registration_data_size);
        const auto bytes = std::span { data }.first(size);
        if (!memory_.copy_out(registers[3], bytes)) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }
        registration.dispatch_queue_offset =
            mach_support::read_little_word(data, 8U);
        const auto version = mach_support::read_little_word(data, 0U);
        const auto tsd = mach_support::read_little_word(data, 24U);
        if (contract.registration_data_size > 24U && version > 24U &&
            tsd < registration.pthread_size)
            registration.tsd_offset = tsd;
        const auto processor = static_cast<std::uint32_t>(cpu.processor_id());
        const auto statistics =
            thread_statistics_query_
                ? thread_statistics_query_(process_.pid, processor)
                : std::nullopt;
        auto qos = statistics ? statistics->scheduling.requested_qos
                              : XnuThreadQosPolicy { };
        if (qos.tier == XnuQosTier::Unspecified)
            qos = process_.execution_policy->main_thread_qos();
        const std::array<std::uint32_t, 2> policy { static_cast<std::uint32_t>(
                                                        qos.tier),
            static_cast<std::uint32_t>(qos.relative_priority) };
        if (!thread_policy_handler_ ||
            !thread_policy_handler_(process_.pid, processor,
                darwin::mach::thread_policy::qos_policy, policy)) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }
        // pthread encodes tiers as class bits and zero relative priority as ff.
        auto class_bit = 1U << (static_cast<std::uint32_t>(qos.tier) - 1U);
        if (contract.priority_encoding ==
            PthreadPriorityEncoding::FineClassBitsV2)
            class_bit >>= 1U;
        mach_support::write_little_word(
            data, 0U, contract.registration_data_size);
        mach_support::write_little_word(data, 4U, 0U);
        mach_support::write_little_word(data, 16U, (class_bit << 8U) | 0xffU);
        mach_support::write_little_word(data, 20U, 0U);
        if (!memory_.copy_to_user(registers[3], bytes)) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }
    }
    if (!pthread_runtime_.register_process(registration)) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return true;
    }
    output_.write("[pthread] register pid=" + std::to_string(process_.pid) +
                  " pthread-size=" + std::to_string(registration.pthread_size) +
                  " dispatch-queue-offset=" +
                  std::to_string(registration.dispatch_queue_offset) + "\n");
    // Split libpthread stores the positive registration result as its
    // kernel capability word. libdispatch requires fine-priority workqueues
    // before installing its worker callback.
    bsd_success(cpu, contract.registration_features);
    return true;
}

bool CompatibilityKernel::set_bsd_pthread_qos(Cpu& cpu)
{
    const auto& contract = shared_state_->pthread_contract;
    auto& registers = cpu.registers();
    if (registers[3] != 0U) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return true;
    }
    const auto encoded = memory_.read32(registers[2]);
    if (!encoded) {
        bsd_error(cpu, bsd_support::bad_address);
        return true;
    }
    std::optional<std::pair<std::uint32_t, std::uint32_t>> target;
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto object = mach_support::resolve_name_with_right(
            *shared_state_, process_.pid, registers[1], xnu::ipc::Right::Send);
        if (object)
            target = mach_support::find_thread_owner(*shared_state_, *object);
    }
    if (!target) {
        bsd_error(cpu, darwin::error::no_such_process);
        return true;
    }
    const auto processor = static_cast<std::uint32_t>(cpu.processor_id());
    const auto statistics =
        thread_statistics_query_
            ? thread_statistics_query_(process_.pid, processor)
            : std::nullopt;
    if (target->first != process_.pid || target->second != processor ||
        (statistics && statistics->scheduling.requested_qos.tier ==
                           XnuQosTier::Unspecified)) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return true;
    }
    const auto qos = PthreadPriority::decode_requested_qos(contract, *encoded);
    const std::array<std::uint32_t, 2> policy { static_cast<std::uint32_t>(
                                                    qos.tier),
        static_cast<std::uint32_t>(qos.relative_priority) };
    if (qos.tier == XnuQosTier::Unspecified || !qos.valid() ||
        !thread_policy_handler_ ||
        !thread_policy_handler_(process_.pid, processor,
            darwin::mach::thread_policy::qos_policy, policy)) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return true;
    }
    if (auto worker = pthread_runtime_.worker(processor)) {
        const auto decoded =
            PthreadPriority::decode_dispatch_workqueue_priority(
                contract, *encoded);
        pthread_runtime_.mark_worker_running(
            processor, decoded->queue, decoded->overcommit);
    }
    bsd_success(cpu, 0);
    if (scheduler_preemption_query_ && scheduler_preemption_query_(processor))
        cpu.request_guest_preemption();
    return true;
}

} // namespace ilemu
