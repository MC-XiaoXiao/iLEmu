// SPDX-License-Identifier: MPL-2.0
#include <app/mig_command.hpp>
#include <device_state/darwin_kernel_configuration.hpp>
#include <foundation/output.hpp>
#include <iomanip>
#include <mach/xnu_mig_reference.hpp>
#include <sstream>
#include <stdexcept>
namespace ilemu {
namespace {
    using namespace xnu::mig::reference;
    void write_layout(
        std::ostream& out, std::string_view side, const Layout* layout)
    {
        out << "layout: " << side;
        if (!layout) {
            out << " absent=1" << std::endl;
            return;
        }
        out << " absent=0 complex=" << layout->complex
            << " descriptors=" << layout->descriptors
            << " size=" << layout->size << " min-size=" << layout->minimum_size
            << std::endl;
        for (const auto& f : layout->fields) {
            out << "field: " << std::quoted(f.name) << " kind=" << f.kind
                << " offset=" << f.offset << " min-offset=" << f.minimum_offset
                << " size=" << f.size << " count=" << f.count
                << " bits=" << f.bits << " descriptors=" << f.descriptors
                << " variable=" << f.variable << " type=" << std::quoted(f.type)
                << " c-type=" << std::quoted(f.c_type)
                << " count-field=" << std::quoted(f.count_field)
                << " sent=" << std::quoted(f.sent_disposition)
                << " received=" << std::quoted(f.received_disposition)
                << std::endl;
        }
    }
    void write_catalog(
        std::ostream& out, std::string_view name, bool validate_only)
    {
        const Catalog catalog { name };
        if (!catalog.valid())
            throw std::runtime_error(
                "invalid MIG reference catalog: " + std::string { name });
        const auto& profile = *catalog.profile();
        out << "profile: " << profile.name << std::endl
            << "source: " << profile.source << std::endl;
        std::size_t present = 0, absent = 0;
        for (const auto& sub : profile.subsystems) {
            if (!validate_only)
                out << "subsystem: " << sub.name << " base=" << sub.base
                    << " end=" << sub.base + sub.slots.size()
                    << " reference-end=" << sub.reference_end << std::endl;
            for (std::size_t i = 0; i < sub.slots.size(); ++i) {
                const auto id = sub.base + static_cast<std::uint32_t>(i);
                const auto result = catalog.resolve(id);
                result.routine ? ++present : ++absent;
                if (validate_only)
                    continue;
                out << "slot: " << id
                    << " reference=" << (result.routine ? "present" : "absent");
                if (!result.routine) {
                    out << std::endl;
                    continue;
                }
                const auto& r = *result.routine;
                out << " name=" << r.name << " reply-id=" << r.reply_identifier
                    << std::endl;
                write_layout(out, "request", r.request);
                write_layout(out, "reply", r.reply);
            }
        }
        out << "present: " << present << std::endl
            << "absent: " << absent << std::endl
            << "validation: ok" << std::endl;
    }
}
void inspect_mig(const std::optional<std::filesystem::path>& rootfs,
    const std::optional<std::string>& ios_build, Output& output, bool all,
    bool validate_only)
{
    if (all && (rootfs || ios_build))
        throw std::invalid_argument(
            "mig --all cannot be combined with --rootfs or --ios-build");
    if (!all && !rootfs && !ios_build)
        throw std::invalid_argument(
            "mig requires --all, --rootfs DIR or --ios-build CODE");
    std::ostringstream text;
    text << "mig-reference-schema: 1" << std::endl
         << "scope: public-XNU ARM32 evidence; reference presence is not "
            "runtime support"
         << std::endl
         << "overlay: ILP32 machine_types; THREAD_STATE_MAX=144; firmware "
            "contracts take precedence"
         << std::endl;
    if (all) {
        for (const auto& entry : darwin_configurations())
            write_catalog(text, entry.name, validate_only);
    } else {
        const auto config = resolve_darwin_configuration(
            rootfs.value_or(std::filesystem::path { }), ios_build);
        write_catalog(text, config.abi_name, validate_only);
    }
    output.write(text.str());
}
}
