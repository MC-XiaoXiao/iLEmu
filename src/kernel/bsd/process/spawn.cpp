// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Decode guest posix_spawn attributes and schedule child process
// creation.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-1228.15.4/bsd/kern/kern_exec.c

#include "kernel/graphics_services_input.hpp"
#include "kernel/kernel.hpp"
#include "foundation/performance.hpp"
#include "foundation/application_path.hpp"
#include "../../mach/transport/message_trace.hpp"
#include "../../mach/support.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "../support.hpp"

namespace ilemu {
namespace {

    constexpr std::uint32_t posix_spawn_syscall = 244U;
    constexpr std::uint16_t posix_spawn_setsigdef = 0x0004U;
    constexpr std::uint16_t posix_spawn_setsigmask = 0x0008U;
    constexpr std::uint16_t posix_spawn_setexec = 0x0040U;
    constexpr std::uint16_t posix_spawn_start_suspended = 0x0080U;
    constexpr std::uint32_t maximum_vector_entries = 4096U;
    constexpr std::uint32_t maximum_string_size = 64U * 1024U;
    constexpr std::uint32_t maximum_port_actions = 256U;

    // Early ARM libc passes a four-word descriptor. Port actions extend that
    // wire object to six words in the later spawn contract (XNU1228+).
    struct SpawnDescriptorProfile {
        bool port_actions;
        bool process_type;
        bool qos_clamp;
        std::int32_t maximum_role;
        bool persona;

        static constexpr SpawnDescriptorProfile for_version(
            DarwinVersion version)
        {
            return { version >= DarwinGeneration::IphoneOs2,
                version >= DarwinGeneration::Ios7,
                version >= DarwinGeneration::Ios8,
                version >= DarwinGeneration::Ios9
                    ? ProcessPriorityProfile::for_version(version).maximum_role : 0,
                version >= DarwinGeneration::Ios9 };
        }
    };

    struct SpawnPortAction {
        std::uint32_t type { };
        std::uint32_t port_name { };
        std::uint32_t which { };
    };

    // Copy-in holds survive asynchronous image preparation. Pending image
    // callbacks are retired outside mach_mutex; their final owner releases the
    // copied send references even when preparation fails or is canceled.
    class SpawnWatchports {
    public:
        explicit SpawnWatchports(std::shared_ptr<KernelSharedState> state)
            : state_ { std::move(state) } { }
        SpawnWatchports(const SpawnWatchports&) = delete;
        SpawnWatchports& operator=(const SpawnWatchports&) = delete;
        ~SpawnWatchports()
        {
            std::lock_guard lock { state_->mach_mutex };
            for (const auto object : objects)
                mach_support::release_kernel_send_right_locked(*state_, object);
        }
        void append_locked(std::uint32_t object)
        {
            objects.push_back(object);
            mach_support::retain_kernel_send_right_locked(*state_, object);
        }
        std::vector<std::uint32_t> objects;
    private:
        std::shared_ptr<KernelSharedState> state_;
    };

    struct PosixSpawnAttributes {
        bool setexec { };
        bool start_suspended { };
        CompatibilityKernel::SpawnAttributes task;
        std::vector<SpawnPortAction> port_actions;
        std::shared_ptr<SpawnWatchports> watchports;
    };

    std::optional<std::vector<std::string>> read_string_vector(
        const AddressSpace& memory, std::uint32_t address)
    {
        std::vector<std::string> values;
        if (address == 0)
            return values;
        for (std::uint32_t index = 0; index < maximum_vector_entries; ++index) {
            const auto pointer = memory.read32(address + index * 4U);
            if (!pointer)
                return std::nullopt;
            if (*pointer == 0)
                return values;
            const auto value =
                memory.read_c_string(*pointer, maximum_string_size);
            if (!value)
                return std::nullopt;
            values.push_back(*value);
        }
        return std::nullopt;
    }

    std::optional<PosixSpawnAttributes> read_spawn_attributes(
        const AddressSpace& memory, std::uint32_t address,
        SpawnDescriptorProfile profile)
    {
        PosixSpawnAttributes result;
        if (address == 0)
            return result;

        const auto attribute_size = memory.read32(address);
        const auto attribute_address = memory.read32(address + 4U);
        if (!attribute_size || !attribute_address) {
            return std::nullopt;
        }
        if (*attribute_size >= sizeof(std::uint16_t) &&
            *attribute_address != 0) {
            const auto flags = memory.read16(*attribute_address);
            if (!flags)
                return std::nullopt;
            result.setexec = (*flags & posix_spawn_setexec) != 0;
            result.start_suspended =
                (*flags & posix_spawn_start_suspended) != 0;
            // XNU _posix_spawnattr begins with flags/padding, sigdefault and
            // sigmask. Read only the fields requested by the flags.
            const auto read_signal_set = [&](std::uint32_t offset)
                -> std::optional<std::uint32_t> {
                if (*attribute_size < offset + sizeof(std::uint32_t) ||
                    *attribute_address >
                        std::numeric_limits<std::uint32_t>::max() -
                            offset - (sizeof(std::uint32_t) - 1U))
                    return std::nullopt;
                return memory.read32(*attribute_address + offset);
            };
            if ((*flags & posix_spawn_setsigdef) != 0U) {
                const auto defaults = read_signal_set(4U);
                if (!defaults)
                    return std::nullopt;
                result.task.defaults = *defaults;
            }
            if ((*flags & posix_spawn_setsigmask) != 0U) {
                result.task.mask = read_signal_set(8U);
                if (!result.task.mask)
                    return std::nullopt;
            }
        }
        if (*attribute_address != 0U) {
            // New fields are appended to the fixed spawnattr ABI. Short older
            // structures retain their zero defaults, as XNU's copyin does.
            if (profile.process_type && *attribute_size >= 40U) {
                const auto encoded = memory.read32(*attribute_address + 36U);
                if (!encoded)
                    return std::nullopt;
                const auto type = (*encoded & 0xf00U) >> 8U;
                if (type <= static_cast<std::uint32_t>(DarwinProcessType::AdaptiveDaemon))
                    result.task.process_type = static_cast<DarwinProcessType>(type);
            }
            if (profile.qos_clamp && *attribute_size >= 88U) {
                const auto clamp = memory.read64(*attribute_address + 80U);
                if (!clamp)
                    return std::nullopt;
                switch (*clamp) {
                case 1: result.task.qos_clamp = XnuQosTier::Utility; break;
                case 2: result.task.qos_clamp = XnuQosTier::Background; break;
                case 3: result.task.qos_clamp = XnuQosTier::Maintenance; break;
                }
            }
            if (profile.maximum_role != 0 && *attribute_size >= 96U) {
                const auto role = memory.read64(*attribute_address + 88U);
                if (!role)
                    return std::nullopt;
                if (*role != 0U && *role <= static_cast<std::uint64_t>(profile.maximum_role))
                    result.task.role = static_cast<DarwinProcessRole>(*role);
            }
        }
        if (!profile.port_actions)
            return result;
        // The 32-bit _posix_spawn_args_desc carries the port-actions block
        // separately from the fixed spawnattr structure. Each action is the
        // six-word _ps_port_action_t defined by XNU's spawn_internal.h.
        const auto port_actions_size = memory.read32(address + 16U);
        const auto port_actions_address = memory.read32(address + 20U);
        if (!port_actions_size || !port_actions_address)
            return std::nullopt;
        if (*port_actions_size != 0U) {
            if (*port_actions_address == 0U || *port_actions_size < 8U)
                return std::nullopt;
            const auto count = memory.read32(*port_actions_address + 4U);
            if (!count || *count > maximum_port_actions ||
                *port_actions_size < 8U + *count * 24U)
                return std::nullopt;
            for (std::uint32_t index = 0; index < *count; ++index) {
                const auto record = *port_actions_address + 8U + index * 24U;
                const auto type = memory.read32(record);
                const auto port_name = memory.read32(record + 8U);
                const auto which = memory.read32(record + 20U);
                if (!type || !port_name || !which)
                    return std::nullopt;
                result.port_actions.push_back({ *type, *port_name, *which });
            }
        }
        return result;
    }

    bool capture_importance_watchports(std::shared_ptr<KernelSharedState> state,
        std::uint32_t caller, PosixSpawnAttributes& attributes)
    {
        if (std::none_of(attributes.port_actions.begin(), attributes.port_actions.end(),
                [](const auto& action) { return action.type == 3U; }))
            return true;
        auto held = std::make_shared<SpawnWatchports>(state);
        bool valid = true;
        {
            std::lock_guard lock { state->mach_mutex };
            for (const auto& action : attributes.port_actions) {
                if (action.type != 3U || action.port_name == xnu::ipc::null_name ||
                    action.port_name == xnu::ipc::dead_name)
                    continue;
                const auto object = mach_support::resolve_name_with_right(
                    *state, caller, action.port_name, xnu::ipc::Right::Send);
                if (!object) {
                    valid = false;
                    break;
                }
                held->append_locked(*object);
            }
        }
        if (valid)
            attributes.watchports = std::move(held);
        return valid;
    }

    void apply_importance_watchports(KernelSharedState& state,
        std::uint32_t target, const PosixSpawnAttributes& attributes)
    {
        if (!attributes.watchports)
            return;
        std::lock_guard lock { state.mach_mutex };
        const auto& policy = *state.processes.at(target).execution_policy;
        if (policy.type() != DarwinProcessType::AdaptiveDaemon)
            return;
        for (const auto object : attributes.watchports->objects) {
            const auto port = state.mach_port_objects.lookup(object);
            if (port)
                static_cast<void>(port->importance->set_watch_task(policy.importance()));
        }
    }

    std::uint32_t apply_special_port_actions(KernelSharedState& state,
        ProcessContext& process, const PosixSpawnAttributes& attributes)
    {
        std::lock_guard mach_lock { state.mach_mutex };
        const auto task_object =
            state.mach_namespaces.resolve(process.pid, process.task_port);
        if (!task_object)
            return bsd_support::invalid_argument;
        for (const auto& action : attributes.port_actions) {
            if (action.type != 0U) // PSPA_SPECIAL
                continue;
            if (!attributes.setexec)
                return 45U; // ENOTSUP
            const auto port_object =
                action.port_name == xnu::ipc::null_name
                    ? std::optional<std::uint32_t> { 0U }
                    : mach_support::resolve_name_with_right(state,
                          process.pid, action.port_name, xnu::ipc::Right::Send);
            if (!port_object)
                return bsd_support::invalid_argument;
            const auto task = state.task_special_ports.find(*task_object);
            const auto previous =
                task != state.task_special_ports.end() &&
                        task->second.contains(action.which)
                    ? task->second.at(action.which)
                    : 0U;
            if (*port_object != 0U && previous != *port_object)
                mach_support::retain_kernel_send_right_locked(state,
                    *port_object);
            if (*port_object == 0U) {
                if (task != state.task_special_ports.end()) {
                    task->second.erase(action.which);
                    if (task->second.empty())
                        state.task_special_ports.erase(task);
                }
            } else {
                state.task_special_ports[*task_object][action.which] =
                    *port_object;
            }
            if (previous != 0U && previous != *port_object)
                mach_support::release_kernel_send_right_locked(state, previous);
            if (action.which == 4U)
                process.bootstrap_port = *port_object != 0U
                    ? action.port_name : xnu::ipc::null_name;
        }
        return 0U;
    }

} // namespace

void CompatibilityKernel::apply_spawn_attributes(
    const SpawnAttributes& attributes)
{
    if (attributes.persona) {
        const auto& persona = *attributes.persona;
        process_.persona = persona.identity;
        process_.uid = process_.effective_uid = persona.uid;
        process_.gid = persona.gid;
        process_.effective_gid = persona.effective_gid;
        process_.groups = persona.groups;
        process_.login_name = persona.login;
        const std::lock_guard lock { shared_state_->mach_mutex };
        auto& record = shared_state_->processes.at(process_.pid);
        record.persona = persona.identity;
        record.uid = record.effective_uid = persona.uid;
        record.gid = persona.gid;
        record.effective_gid = persona.effective_gid;
        record.groups = persona.groups;
    }
    process_.execution_policy->set_spawn_policy(attributes.process_type, attributes.qos_clamp);
    if (attributes.role)
        process_.execution_policy->set_role(*attributes.role);
    if (attributes.mask)
        static_cast<void>(signal_state_.update(0, 3, *attributes.mask,
            DarwinSignalState::Scope::Process));
    for (std::size_t signal = 1; signal < signal_actions_.size(); ++signal) {
        if ((attributes.defaults & (1U << (signal - 1U))) != 0U)
            signal_actions_[signal] = { };
    }
}

bool CompatibilityKernel::dispatch_bsd_process_spawn(
    Cpu& cpu, std::uint32_t number)
{
    if (number != posix_spawn_syscall)
        return false;

    PerformanceLatencyScope total_latency { PerfLatencyKind::PosixSpawnTotal };
    auto& registers = cpu.registers();
    const auto pid_address = registers[0];
    std::optional<std::string> path;
    std::optional<std::vector<std::string>> arguments;
    std::optional<std::vector<std::string>> environment;
    std::optional<PosixSpawnAttributes> attributes;
    {
        PerformanceLatencyScope decode_latency {
            PerfLatencyKind::PosixSpawnDecode
        };
        path = memory_.read_c_string(registers[1]);
        arguments = read_string_vector(memory_, registers[3]);
        environment = read_string_vector(memory_, registers[4]);
        attributes = read_spawn_attributes(memory_, registers[2],
            SpawnDescriptorProfile::for_version(shared_state_->darwin_version));
    }
    if (!path || !arguments || !environment || !attributes ||
        (pid_address == 0 && !attributes->setexec)) {
        bsd_error(cpu, bsd_support::bad_address);
        return true;
    }

    if (registers[2] != 0U &&
        SpawnDescriptorProfile::for_version(shared_state_->darwin_version).persona) {
        if (const auto error = read_spawn_persona(registers[2], attributes->task)) {
            bsd_error(cpu, error);
            return true;
        }
    }
    if (!capture_importance_watchports(shared_state_, process_.pid, *attributes)) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return true;
    }

    // `--suspended` is an application argument used by first-generation
    // SpringBoard's prewarm protocol, not a kernel spawn attribute. The child
    // must run through dyld and suspend itself in userspace. Only the actual
    // POSIX_SPAWN_START_SUSPENDED flag places the initial thread on hold.

    prepare_process_image(cpu, std::move(*path), *arguments, 8,
        [this, pid_address, environment = std::move(*environment),
            attributes = std::move(*attributes), arguments = std::move(*arguments)](Cpu& cpu,
            PreparedProcessImage image) mutable {
        const auto path = image.requested_path;
        const auto trace_launch_intent = [&](std::string_view stage) {
            if (!performance_counters().cpu_source_diagnostics_enabled())
                return;
            const std::lock_guard lock { shared_state_->mach_mutex };
            if (!is_application_executable_path(path) &&
                !shared_state_->foreground_application_attempt_process_id)
                return;
            std::ostringstream trace;
            trace << "[perf] launch-intent action=" << stage
                  << " time-ns=" << mach_transport::flow_timestamp()
                  << " policy-type=" << static_cast<unsigned>(process_.execution_policy->type())
                  << " policy-background=" << process_.execution_policy->qos_policy().background
                  << " policy-donating=" << process_.execution_policy->importance()->donating()
                  << " policy-boosted=" << process_.execution_policy->importance()->boosted()
                  << " pid=" << process_.pid << " parent=" << process_.parent_pid
                  << " path=" << path
                  << " pending=" << shared_state_->pending_application_launch_touch_sequence.value_or(0U)
                  << " incarnation=" << shared_state_->processes.at(process_.pid).incarnation
                  << " gesture-boundary=" << shared_state_->pending_application_launch_incarnation_boundary
                  << " foreground=" << shared_state_->foreground_application_attempt_process_id.value_or(0U)
                  << " type=" << static_cast<unsigned>(attributes.task.process_type)
                  << " role=" << (attributes.task.role ? static_cast<int>(*attributes.task.role) : -1)
                  << " qos-clamp=" << static_cast<unsigned>(attributes.task.qos_clamp);
            output_.line(trace.str());
        };
        if (attributes.setexec) {
            trace_launch_intent("setexec-before");
            if (!exec_handler_ ||
                !exec_handler_(cpu, std::move(image), environment)) {
                bsd_error(cpu, 8); // ENOEXEC
                return;
            }
            if (const auto error = apply_special_port_actions(
                    *shared_state_, process_, attributes); error != 0U) {
                bsd_error(cpu, error);
                return;
            }
            apply_spawn_attributes(attributes.task);
            apply_importance_watchports(*shared_state_, process_.pid, attributes);
            output_.write("[process] spawn-port-actions pid=" +
                std::to_string(process_.pid) + " count=" +
                std::to_string(attributes.port_actions.size()) +
                " bootstrap=" + std::to_string(process_.bootstrap_port) + "\n");
            // SpringBoard's older launch path forks a helper and asks that child to
            // SETEXEC the application. There is no separate child-return path
            // below, so observe the successful PID-bound spawn here after exec has
            // assigned the new process identity.
            graphics_services_input::record_application_spawn(*shared_state_,
                process_.parent_pid, process_.pid, path, arguments,
                scene_coordinator_.get(), true);
            trace_launch_intent("setexec-after");
            performance_counters().record_exec();
            std::ostringstream message;
            message << "[process] spawn-setexec pid=" << process_.pid
                    << " parent=" << process_.parent_pid
                    << " suspended=" << attributes.start_suspended << " " << path
                    << " argv=";
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                if (index != 0)
                    message << ',';
                message << '"' << arguments[index] << '"';
            }
            message << '\n';
            output_.write(message.str());
            cpu.halt(Dynarmic::HaltReason::UserDefined6);
            return;
        }

        std::optional<std::uint32_t> child;
        if (spawn_create_handler_) {
            PerformanceLatencyScope create_latency {
                PerfLatencyKind::PosixSpawnCreate
            };
            child = spawn_create_handler_(cpu);
        } else {
            PerformanceLatencyScope fork_latency {
                PerfLatencyKind::PosixSpawnFork
            };
            child = fork_handler_ ? fork_handler_(cpu) : std::nullopt;
        }
        if (!child) {
            bsd_error(cpu, 11); // EAGAIN
            return;
        }
        performance_counters().record_fork();
        if (!spawn_exec_handler_ ||
            !spawn_exec_handler_(*child, std::move(image), environment,
                attributes.start_suspended, attributes.task)) {
            bsd_error(cpu, 8); // ENOEXEC
            return;
        }
        apply_importance_watchports(*shared_state_, *child, attributes);
        if (scheduler_preemption_query_ &&
            scheduler_preemption_query_(cpu.processor_id())) {
            // A successful non-suspended spawn leaves a new runnable child visible
            // to the scheduler before the parent returns to Guest code. A suspended
            // child is already blocked by spawn_exec_handler_ and therefore does
            // not trigger this query unless another runnable candidate wins
            // ordering.
            cpu.request_guest_preemption();
        }
        graphics_services_input::record_application_spawn(*shared_state_,
            process_.pid, *child, path, arguments, scene_coordinator_.get());
        performance_counters().record_exec();
        if (!memory_.write32(pid_address, *child)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }

        std::ostringstream message;
        message << "[process] spawn parent=" << process_.pid << " child=" << *child
                << " suspended=" << attributes.start_suspended << " " << path
                << " argv=";
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (index != 0)
                message << ',';
            message << '"' << arguments[index] << '"';
        }
        message << '\n';
        output_.write(message.str());
        bsd_success(cpu, 0);
    });
    return true;
}

} // namespace ilemu
