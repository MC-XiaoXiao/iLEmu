// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "device_state/darwin_abi.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ilemu::syscall_routes {
using Domain = DarwinAbiDomain;
enum class Handler {
    BsdProcess,
    BsdProcessSockets,
    BsdPosixSemaphore,
    BsdSignal,
    BsdPlatform,
    BsdFilesystem,
    BsdDescriptorMemory,
    BsdSharedRegion,
    BsdPsynch,
    BsdAio,
    BsdDebug,
    BsdSocket,
    BsdEvents,
    BsdKqueue,
    BsdCodeSigning,
    BsdSecurity,
    BsdAuditSession,
    BsdFileport,
    BsdGuardedFile,
    BsdCoalition,
    BsdNetworkPolicy,
    BsdDirectoryAttributes,
    BsdPthread,
    BsdLedgerInline,
    BsdIoPolicyInline,
    BsdStackSnapshotInline,
    MachInline,
    MachMessage,
    MachThreadSelf,
    MachVmRpc,
    MachPortRpc,
    Count,
};
enum class Contract {
    CurrentDispatcher,
    LaterEpoch,
    ResourceCoalitions,
    Ledger,
    IoPolicy,
    GuardedFdChange,
    Connectx,
    StackSnapshot,
    SemaphoreValue,
    NamedSysctl,
    Psynch,
    LegacySharedRegion,
    SharedRegionSlide,
    PthreadRegisterV1,
    PthreadIdentity,
    PthreadControl,
    MachDirectRpc,
    MachMixedVm,
    MachWideVm,
    MachLegacyInit,
    Count,
};
enum class Cancellation { OriginalEntry, NoCancelAlias, NoCancelEntry };
// First-handler reachability is not a promise of complete syscall support.
enum class Outcome {
    HandlerValidated,
    BsdNosys,
    BsdUnknown,
    MachUnknown,
    MigFallback
};
// Strings refer to static catalog storage. No entry invokes guest code.
struct Entry {
    Domain domain;
    std::uint32_t number;
    std::uint32_t canonical_number;
    std::string_view operation;
    Handler handler;
    Contract contract { Contract::CurrentDispatcher };
    Cancellation cancellation { Cancellation::OriginalEntry };
    Outcome outcome { Outcome::HandlerValidated };
    std::string_view source;
    bool operator==(const Entry&) const = default;
};
struct Replacement {
    Entry previous;
    Entry replacement;
};
class Table {
public:
    // Checked catalog capacities, not assertions about the entire Darwin ABI.
    static constexpr std::size_t bsd_capacity = 512;
    static constexpr std::size_t mach_capacity = 128;
    void bind_new(const Entry& entry);
    void replace_entry(const Entry& expected, const Entry& replacement);
    [[nodiscard]] const Entry* find(Domain domain, std::uint32_t number) const;
    [[nodiscard]] std::span<const std::optional<Entry>> entries(
        Domain domain) const;
    [[nodiscard]] std::span<const Replacement> replacements() const
    {
        return replacements_;
    }
    void validate() const;

private:
    std::optional<Entry>& slot(Domain domain, std::uint32_t number);
    std::array<std::optional<Entry>, bsd_capacity> bsd_ { };
    std::array<std::optional<Entry>, mach_capacity> mach_ { };
    std::vector<Replacement> replacements_;
};
[[nodiscard]] std::string_view handler_name(Handler handler);
[[nodiscard]] std::string_view contract_name(Contract contract);
[[nodiscard]] std::string_view domain_name(Domain domain);
[[nodiscard]] std::string_view outcome_name(Outcome outcome);
[[nodiscard]] Table build(const DarwinAbi& abi);
} // namespace ilemu::syscall_routes
