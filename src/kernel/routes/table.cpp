// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/syscall_routes.hpp"
#include <stdexcept>
#include <string>

namespace ilemu::syscall_routes {
namespace {
    std::string identity(const Entry& e)
    {
        return std::string(domain_name(e.domain)) + ":" +
               std::to_string(e.number) + " " + std::string(e.operation) +
               " [" + std::string(contract_name(e.contract)) + "] -> " +
               std::string(handler_name(e.handler)) + "/" +
               std::string(outcome_name(e.outcome)) +
               " canonical=" + std::to_string(e.canonical_number) +
               " cancellation=" +
               std::to_string(static_cast<int>(e.cancellation)) +
               " source=" + std::string(e.source);
    }
    void check_entry(const Entry& e)
    {
        const bool bsd = e.domain == Domain::BsdSyscall;
        if (!bsd && e.domain != Domain::MachTrap)
            throw std::invalid_argument(
                "route domain must be BSD or normalized Mach; MIG/ARM are "
                "separate");
        const auto capacity = bsd ? Table::bsd_capacity : Table::mach_capacity;
        if (e.number >= capacity || e.canonical_number >= capacity)
            throw std::out_of_range(
                "route number/canonical outside catalog: " + identity(e));
        if (e.operation.empty() || e.source.empty() ||
            handler_name(e.handler) == "invalid" ||
            contract_name(e.contract) == "invalid" ||
            outcome_name(e.outcome) == "invalid")
            throw std::invalid_argument(
                "invalid route identity: " + identity(e));
        if (bsd != (e.handler < Handler::MachInline))
            throw std::invalid_argument(
                "handler belongs to another domain: " + identity(e));
        if (e.cancellation != Cancellation::OriginalEntry &&
            e.cancellation != Cancellation::NoCancelAlias &&
            e.cancellation != Cancellation::NoCancelEntry)
            throw std::invalid_argument(
                "invalid cancellation contract: " + identity(e));
        if ((!bsd && (e.cancellation != Cancellation::OriginalEntry ||
                         e.outcome == Outcome::BsdNosys ||
                         e.outcome == Outcome::BsdUnknown)) ||
            (bsd && (e.outcome == Outcome::MachUnknown ||
                        e.outcome == Outcome::MigFallback)))
            throw std::invalid_argument(
                "outcome/cancellation belongs to another domain: " +
                identity(e));
        if ((e.cancellation == Cancellation::NoCancelAlias) !=
            (e.number != e.canonical_number))
            throw std::invalid_argument(
                "alias must retain a distinct original number: " + identity(e));
    }
}
std::span<const std::optional<Entry>> Table::entries(Domain domain) const
{
    if (domain == Domain::BsdSyscall)
        return bsd_;
    if (domain == Domain::MachTrap)
        return mach_;
    throw std::invalid_argument("MIG and ARM fast traps are separate domains");
}
std::optional<Entry>& Table::slot(Domain domain, std::uint32_t number)
{
    if (number >= entries(domain).size())
        throw std::out_of_range(std::string(domain_name(domain)) + ":" +
                                std::to_string(number) + " outside catalog");
    return domain == Domain::BsdSyscall ? bsd_[number] : mach_[number];
}
const Entry* Table::find(Domain domain, std::uint32_t number) const
{
    const auto view = entries(domain);
    if (number >= view.size())
        throw std::out_of_range(std::string(domain_name(domain)) + ":" +
                                std::to_string(number) + " outside catalog");
    return view[number] ? &*view[number] : nullptr;
}
void Table::bind_new(const Entry& e)
{
    check_entry(e);
    auto& destination = slot(e.domain, e.number);
    if (destination)
        throw std::invalid_argument("duplicate binding: existing " +
                                    identity(*destination) + "; incoming " +
                                    identity(e));
    destination = e;
}
void Table::replace_entry(const Entry& expected, const Entry& replacement)
{
    check_entry(expected);
    check_entry(replacement);
    if (expected.domain != replacement.domain ||
        expected.number != replacement.number)
        throw std::invalid_argument(
            "replacement changes route key: " + identity(expected) +
            "; incoming " + identity(replacement));
    auto& destination = slot(expected.domain, expected.number);
    if (!destination || *destination != expected)
        throw std::invalid_argument(
            "replacement identity mismatch: expected " + identity(expected) +
            "; actual " + (destination ? identity(*destination) : "unbound") +
            "; incoming " + identity(replacement));
    if (expected == replacement)
        throw std::invalid_argument(
            "replacement must change identity: " + identity(expected));
    replacements_.push_back({ expected, replacement });
    destination = replacement;
}
void Table::validate() const
{
    for (const auto domain : { Domain::BsdSyscall, Domain::MachTrap }) {
        for (const auto& slot : entries(domain)) {
            if (!slot)
                continue;
            const auto& e = *slot;
            check_entry(e);
            if (e.cancellation != Cancellation::NoCancelAlias)
                continue;
            const auto* target = find(domain, e.canonical_number);
            if (!target ||
                target->cancellation == Cancellation::NoCancelAlias ||
                target->handler != e.handler ||
                target->contract != e.contract || target->outcome != e.outcome)
                throw std::invalid_argument(
                    "invalid alias target: " + identity(e));
        }
    }
}
} // namespace ilemu::syscall_routes
