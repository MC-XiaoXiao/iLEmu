// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "app/routes_command.hpp"
#include "device_state/darwin_kernel_configuration.hpp"
#include "foundation/output.hpp"
#include "kernel/syscall_routes.hpp"
#include "kernel/bsd_route_policy.hpp"
#include "kernel/mach_route_policy.hpp"
#include <sstream>
#include <stdexcept>
namespace ilemu {
namespace {
    using namespace syscall_routes;
    void write_routes(std::ostringstream& text, std::string_view name,
        const DarwinAbi& abi, std::string_view darwin_release, bool validate_only)
    {
        const auto table = build(abi);
        const BsdRoutePolicy policy { abi, darwin_release };
        text << "profile: " << name << std::endl;
        text << "darwin-release: " << darwin_release << std::endl;
        std::array<std::size_t, 3> dispositions { };
        for (std::uint32_t number = 0; number < Table::bsd_capacity; ++number) {
            const auto resolved =
                policy.resolve(table.find(Domain::BsdSyscall, number), number);
            ++dispositions[static_cast<std::size_t>(resolved.disposition)];
            if (validate_only)
                continue;
            text << "bsd-policy:" << number
                 << " canonical=" << resolved.canonical
                 << " disposition=" << bsd_disposition_name(resolved.disposition)
                 << " reference=" << bsd_reference_name(resolved.reference)
                 << " fallback=ENOSYS"
                 << " fallback-sigsys=" << resolved.send_sigsys
                 << " trace-unknown=" << resolved.trace_unknown
                 << " reference-gated=" << resolved.reference_gated
                 << " source=kernel/xnu_reference_syscalls.hpp" << std::endl;
        }
        text << "bsd-policy-slots: " << Table::bsd_capacity << std::endl
             << "bsd-policy-handler-validates: " << dispositions[0] << std::endl
             << "bsd-policy-deferred: " << dispositions[1] << std::endl
             << "bsd-policy-errno-stub: " << dispositions[2] << std::endl;
        const MachRoutePolicy mach_policy { name };
        std::array<std::size_t, 3> mach_dispositions { };
        for (std::uint32_t trap = 0; trap < Table::mach_capacity; ++trap) {
            const auto resolved = mach_policy.resolve(table.find(Domain::MachTrap, trap), trap);
            ++mach_dispositions[static_cast<std::size_t>(resolved.disposition)];
            if (validate_only) continue;
            text << "mach-policy:" << trap
                 << " disposition=" << mach_disposition_name(resolved.disposition)
                 << " reference=" << mach_reference_name(resolved.reference)
                 << " fallback-result=" << resolved.fallback_result
                 << " trace-unknown=" << resolved.trace_unknown
                 << " source=" << mach_policy.source() << std::endl;
        }
        text << "mach-policy-slots: " << Table::mach_capacity << std::endl
             << "mach-policy-handler-validates: " << mach_dispositions[0] << std::endl
             << "mach-policy-deferred: " << mach_dispositions[1] << std::endl
             << "mach-policy-errno-stub: " << mach_dispositions[2] << std::endl;
        text << "send-sigsys: " << abi.capabilities.send_sigsys << std::endl;
        for (const auto domain : { Domain::BsdSyscall, Domain::MachTrap }) {
            std::size_t count = 0;
            for (const auto& slot : table.entries(domain)) {
                if (!slot)
                    continue;
                ++count;
                if (validate_only)
                    continue;
                const auto& e = *slot;
                const auto outcome = domain == Domain::BsdSyscall &&
                        policy.resolve(&e, e.number).reference_gated
                    ? Outcome::BsdNosys : e.outcome;
                text << domain_name(domain) << ':' << e.number
                     << " operation=" << e.operation
                     << " canonical=" << e.canonical_number
                     << " contract=" << contract_name(e.contract)
                     << " handler=" << handler_name(e.handler)
                     << " outcome=" << outcome_name(outcome)
                     << " cancellation="
                     << (e.cancellation == Cancellation::NoCancelAlias
                                ? "nocancel-alias"
                            : e.cancellation == Cancellation::NoCancelEntry
                                ? "nocancel-entry"
                                : "original-entry")
                     << " source=" << e.source << std::endl;
            }
            text << domain_name(domain) << "-entries: " << count << std::endl;
        }
        for (const auto& replacement : table.replacements()) {
            const auto& old = replacement.previous;
            const auto& next = replacement.replacement;
            text << "replace " << domain_name(old.domain) << ':' << old.number
                 << ' ' << old.operation << '/' << contract_name(old.contract)
                 << '/' << handler_name(old.handler) << " -> " << next.operation
                 << '/' << contract_name(next.contract) << '/'
                 << handler_name(next.handler) << std::endl;
        }
        text << "validation: ok" << std::endl;
    }
}
void inspect_routes(const std::optional<std::filesystem::path>& rootfs,
    const std::optional<std::string>& ios_build, Output& output, bool all,
    bool validate_only)
{
    if (all && (rootfs || ios_build))
        throw std::invalid_argument(
            "routes --all cannot be combined with --rootfs or --ios-build");
    if (!all && !rootfs && !ios_build)
        throw std::invalid_argument(
            "routes requires --all, --rootfs DIR or --ios-build CODE");
    std::ostringstream text;
    text << "syscall-route-schema: 3" << std::endl
         << "mode: executable BSD and Mach catalogs"
         << std::endl
         << "scope: first-handler routing; handler-validates is not a full "
            "implementation guarantee"
         << std::endl
         << "domains: BSD number; normalized positive Mach trap; excludes "
            "ARM-fast/MIG/IOKit"
         << std::endl
         << "bsd-policy: all normalized dispatch slots; SVC syscall(0) decodes "
            "the effective number before this layer; handler-validates is not "
            "an implemented-coverage claim; fallback fields also apply to "
            "handler-level nosys"
         << std::endl
         << "mach-policy: all normalized slots; slot0 is not the BSD SVC0 "
            "indirect entry; audited bindings precede public XNU evidence; "
            "deferred calls retain their explicit fallback, not full support"
         << std::endl
         << "unbound-bsd: trace-unknown+ENOSYS; SIGSYS only where the "
            "reference XNU slot is nosys (kernel/xnu_reference_syscalls.hpp); "
            "unbound-mach: trace-unknown+invalid-argument"
         << std::endl
         << "nocancel: original numbers retained; current ungated "
            "canonicalization; no new cancellation implementation"
         << std::endl;
    if (all) {
        for (const auto& entry : darwin_configurations())
            write_routes(text, entry.name, entry.abi, entry.darwin_release, validate_only);
    } else {
        const auto configuration = resolve_darwin_configuration(
            rootfs.value_or(std::filesystem::path { }), ios_build);
        write_routes(
            text, configuration.abi_name, configuration.abi,
            configuration.identity.operating_system_release, validate_only);
    }
    output.write(text.str());
}
}
