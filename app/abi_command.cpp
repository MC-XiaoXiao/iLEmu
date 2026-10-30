// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Report the selected firmware ABI and device capabilities from the
// CLI.

#include "app/abi_command.hpp"

#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "device_state/darwin_kernel_configuration.hpp"
#include "foundation/output.hpp"

namespace ilemu {
namespace {

    template <class Enum>
    std::string_view choice(Enum value,
        std::initializer_list<std::string_view> names)
    {
        const auto index = static_cast<std::size_t>(value);
        return index < names.size() ? names.begin()[index] : "unknown";
    }

    void write_abi_fields(std::ostream& text, const DarwinAbi& abi)
    {
        text << "epoch: " << choice(abi.abi_epoch,
                { "unknown", "iphone-os-1", "iphone-os-2", "iphone-os-3",
                    "darwin-10", "darwin-11", "darwin-13", "later" }) << '\n'
         << "pthread: " << choice(abi.pthread_abi,
                { "mach-threads", "bsd-register-v1", "bsd-register-v1-tsd",
                    "bsd-register-v1-tsd-four-priority",
                    "bsd-register-v1-expanded-tsd-four-priority",
                    "bsd-register-v1-expanded-tsd-fine-priority",
                    "bsd-register-v1-expanded-tsd-kevent-workqueues",
                    "bsd-register-v2" })
         << '\n'
         << "apple80211: " << choice(abi.apple80211_ioctl,
                { "aligned-network-record", "compact-network-record" }) << '\n'
         << "io-connect-method: " << choice(abi.io_connect_method,
                { "natural32-scalar-structure", "natural32-structure-scalar",
                    "mach-vm64-structure-scalar" }) << '\n'
         << "notify-state: " << choice(abi.notify_state_abi,
                { "native-server-tokens", "bootstrap-aware-server-tokens" })
         << '\n'
         << "initial-apple-vector: " << choice(abi.initial_apple_vector_abi,
                { "keyed-path", "legacy-path" }) << '\n'
         << "shared-region: " << choice(abi.shared_region_abi,
                { "relocatable", "fixed-with-slide-info-v1" }) << '\n'
         << "mach-kernel-rpc: " << choice(abi.mach_kernel_rpc,
                { "mig-only", "direct-vm-port-traps-v1",
                    "direct-wide-vm-port-traps" }) << '\n'
         << "arm-commpage: " << choice(abi.arm_commpage,
                { "legacy-address", "high-address", "high-data-address" }) << '\n'
         << "psynch: " << choice(abi.psynch_abi,
                { "unsupported", "arm32-generation-v1" }) << '\n'
         << "psynch-rw-conversion: " << choice(abi.psynch_rw_conversion,
                { "generation-queues", "unused-stubs" }) << '\n'
         << "semaphore-wait: " << choice(abi.semaphore_wait_abi,
                { "inline-seconds32", "inline-seconds64" }) << '\n'
         << "iokit-matching: " << choice(abi.iokit_matching_rpc,
                { "plural-iterator", "inline-single-service-v1",
                    "inline-single-service-after-variable-output" }) << '\n'
         << "address-layout: " << choice(abi.address_layout,
                { "classic-arm", "expanded-arm-shared-region", "arm-shared-region-at-512mib",
                    "arm-shared-region-at-416mib" }) << '\n'
         << "sysctl-by-name: " << choice(abi.sysctl_by_name_abi,
                { "legacy-semaphore-value", "named-sysctl-at-274" }) << '\n'
         << "sysctl-transfer: " << choice(abi.sysctl_transfer,
                { "legacy-kernel-handlers", "oid-sized-queries",
                    "oid-natural-size-queries" }) << '\n'
         << "exception-ports: " << choice(abi.exception_port_abi,
                { "through-crash", "through-guard", "through-corpse-notify" }) << '\n'
         << "coalition: " << choice(abi.coalition_abi,
                { "unsupported", "resource-coalitions", "resource-and-jetsam-coalitions" }) << '\n'
         << "task-trace-memory: " << choice(abi.task_trace_memory,
                { "unsupported", "registration", "process-inspection" }) << '\n'
         << "mach-voucher: " << choice(abi.mach_voucher_abi,
                { "unsupported", "host-create-inline-recipes",
                    "host-create-direct-traps" }) << '\n'
         << "sandbox: " << choice(abi.sandbox_abi,
                { "natural32-arguments", "wide64-arguments" }) << '\n'
         << "mach-port-context: " << choice(abi.mach_port_context,
                { "natural32", "wide64" }) << '\n'
         << "mach-vm-address: " << choice(abi.mach_vm_address,
                { "natural32", "wide64" }) << '\n'
         << "stack-snapshot: " << choice(abi.stack_snapshot_abi,
                { "unsupported", "legacy-four-arguments", "legacy-five-arguments" }) << '\n'
         << "framebuffer-registry: " << choice(abi.framebuffer_registry,
                { "device-specific-class", "unified-clcd-class" }) << '\n'
         << "memory-status-priority: " << choice(abi.memory_status_priority,
                { "unsupported", "signed-priority", "priority-bands" }) << '\n'
         << "hid-digitizer: " << choice(abi.hid_digitizer,
                { "collection-contact-changes", "child-contact-changes", "collection-contact-changes-with-edge-origin" }) << '\n'
         << "io-service-state: " << choice(abi.io_service_state,
                { "state-only", "state-with-busy-accounting" }) << '\n'
         << "mach-descriptor-copyin: " << choice(abi.mach_descriptor_copyin,
                { "reverse-compact-descriptors", "forward-expanded-descriptors" }) << '\n'
         << "task-suspension: " << choice(abi.task_suspension,
                { "shared-user-count", "protected-pid-hold", "resume-port-tokens" }) << '\n'
         << "task-information: " << choice(abi.task_information,
                { "basic", "resident-peak", "dyld-address-pair", "dyld-with-format", "unified-basic" }) << '\n'
         << "resource-accounting: " << choice(abi.resource_accounting,
                { "cpu-time", "resident-peak" }) << '\n'
         << "child-wait: " << choice(abi.child_wait,
                { "stop-option", "expanded-stop-mask", "child-identity" }) << '\n'
         << "task-special-ports: " << choice(abi.task_special_ports,
                { "ledger", "security-ledger-automount", "security-ledger", "security", "security-debug" }) << '\n'
         << "mach-port-requests: " << choice(abi.mach_port_requests,
                { "dead-name-requests", "send-possible-requests" }) << '\n'
         << "mach-port-attribute-array: " << choice(abi.mach_port_attribute_array,
                { "receive-status-10", "extended-status-17" }) << '\n'
         << "mach-port-ownership: " << choice(abi.mach_port_ownership,
                { "fixed-receiver", "temporary-receiver" }) << '\n'
         << "mach-port-status: " << choice(abi.mach_port_status,
                { "reserved-flags", "importance-and-guards" }) << '\n'
         << "mach-message-header: " << choice(abi.mach_message_header,
                { "checked-port-bytes", "masked-port-bytes", "masked-port-fields" }) << '\n'
         << "thread-timeshare: " << choice(abi.thread_timeshare,
                { "nonzero-boolean", "canonical-boolean" }) << '\n'
         << "socket-linger: " << choice(abi.socket_linger,
                { "option-mask", "canonical-boolean" }) << '\n'
         << "socket-accept-flags: " << choice(abi.socket_accept_flags,
                { "common-options", "extended-flags" }) << '\n'
         << "socket-accept-failure: " << choice(abi.socket_accept_failure,
                { "requeue", "discard" }) << '\n'
         << "file-open: " << choice(abi.file_open,
                { "legacy", "data-synchronized", "close-on-exec" }) << '\n'
         << "descriptor-passing: " << choice(abi.descriptor_passing,
                { "any-file", "sendable-file-types" }) << '\n'
         << "thread-priority-floor: " << choice(abi.thread_priority_floor,
                { "unrestricted", "throttle" }) << '\n'
         << "kevent-abi: " << choice(abi.kevent_abi,
                { "legacy", "quality-of-service" }) << '\n'
         << "gettimeofday-abi: " << choice(abi.gettimeofday_abi,
                { "register-timeval", "copyout-timeval-and-absolute-time" }) << '\n'
         << std::boolalpha
         << "send-sigsys: " << abi.capabilities.send_sigsys << '\n'
         << "cache-trap-grants-execute: "
         << abi.capabilities.arm_cache_trap_grants_execute << '\n'
         << "getentropy: " << abi.capabilities.getentropy << '\n'
         << "user-lock-wait: " << abi.capabilities.user_lock_wait << '\n'
         << "file-signature-end-offset: "
         << abi.capabilities.file_signature_end_offset << '\n';
    }

} // namespace

void inspect_abi(const std::optional<std::filesystem::path>& rootfs,
    const std::optional<std::string>& ios_build, Output& output, bool all)
{
    if (all) {
        if (rootfs || ios_build)
            throw std::invalid_argument {
                "abi --all cannot be combined with --rootfs or --ios-build" };
        std::ostringstream text;
        text << "abi-catalog-schema: 1" << std::endl;
        for (const auto& entry : darwin_configurations()) {
            text << std::endl << "abi: " << entry.name << std::endl
                 << "os-release: " << entry.darwin_release << std::endl;
            write_abi_fields(text, entry.abi);
        }
        output.write(text.str());
        return;
    }
    if (!rootfs && !ios_build) {
        output.line("Use --rootfs DIR to inspect the firmware's "
                    "Darwin/ABI configuration.");
        output.line("Use --ios-build CODE (e.g. 9A334) to override "
                    "firmware metadata.");
        output.line("Known Darwin/ABI configurations:");
        for (const auto& entry : darwin_configurations())
            output.line("  " + std::string { entry.name });
        return;
    }
    const auto configuration = resolve_darwin_configuration(
        rootfs.value_or(std::filesystem::path { }), ios_build);
    const auto& abi = configuration.abi;
    const auto& identity = configuration.identity;
    std::ostringstream text;
    text << "abi: " << configuration.abi_name << '\n'
         << "source: " << darwin_abi_source_name(configuration.abi_source)
         << '\n' << "source-detail: " << configuration.abi_source_detail << '\n'
         << "activation-hardware-model: "
         << configuration.activation_profile.name() << '\n'
         << "kernel: " << identity.name << '\n'
         << "os-type: " << identity.operating_system_type << '\n'
         << "os-release: " << identity.operating_system_release << '\n'
         << "os-revision: " << identity.operating_system_revision << '\n'
         << "os-version: " << identity.build_version << '\n'
         << "kernel-version: " << identity.version << '\n';
    write_abi_fields(text, abi);
    output.write(text.str());
}

} // namespace ilemu
