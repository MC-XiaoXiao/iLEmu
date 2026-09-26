// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "app/routes_command.hpp"
#include "device_state/darwin_kernel_configuration.hpp"
#include "foundation/output.hpp"
#include "kernel/syscall_routes.hpp"
#include <sstream>
#include <stdexcept>
namespace ilemu {
namespace {
    using namespace syscall_routes;
    void write_routes(std::ostringstream& text, std::string_view name,
        const DarwinAbi& abi, bool validate_only)
    {
        const auto table = build(abi);
        text << "profile: " << name << std::endl;
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
                text << domain_name(domain) << ':' << e.number
                     << " operation=" << e.operation
                     << " canonical=" << e.canonical_number
                     << " contract=" << contract_name(e.contract)
                     << " handler=" << handler_name(e.handler)
                     << " outcome=" << outcome_name(e.outcome)
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
    text << "syscall-route-schema: 1" << std::endl
         << "mode: inspection-only; execution remains on existing dispatch"
         << std::endl
         << "scope: first-handler routing; handler-validates is not a full "
            "implementation guarantee"
         << std::endl
         << "domains: BSD number; normalized positive Mach trap; excludes "
            "ARM-fast/MIG/IOKit"
         << std::endl
         << "unbound-bsd: trace-unknown+nosys-policy; unbound-mach: "
            "trace-unknown+invalid-argument"
         << std::endl
         << "nocancel: original numbers retained; current ungated "
            "canonicalization; no new cancellation implementation"
         << std::endl;
    if (all) {
        for (const auto& entry : darwin_configurations())
            write_routes(text, entry.name, entry.abi, validate_only);
    } else {
        const auto configuration = resolve_darwin_configuration(
            rootfs.value_or(std::filesystem::path { }), ios_build);
        write_routes(
            text, configuration.abi_name, configuration.abi, validate_only);
    }
    output.write(text.str());
}
}
