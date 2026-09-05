#include "kernel/darwin_sysctl.hpp"

#include <array>

namespace ilemu::darwin::sysctl {
namespace {

    constexpr std::uint32_t readable = 0x80000000U;
    constexpr std::uint32_t writable = 0x40000000U;
    constexpr std::uint32_t integer_type = 2U;
    constexpr std::uint32_t string_type = 3U;
    constexpr std::uint32_t quad_type = 4U;

    struct NamedObject {
        std::string_view name;
        std::uint32_t control;
        std::uint32_t selector;
        std::uint32_t kind { readable | integer_type };
        std::string_view format { "I" };
    };

    // Keep this table limited to nodes for which the compatibility kernel has
    // a value handler.
    constexpr std::array named_objects {
        NamedObject {
            "kern.ostype", control_kernel, kernel_operating_system_type,
            readable | string_type, "A" },
        NamedObject {
            "kern.osrelease", control_kernel, kernel_operating_system_release,
            readable | string_type, "A" },
        NamedObject { "kern.osrevision", control_kernel,
            kernel_operating_system_revision },
        NamedObject { "kern.version", control_kernel, kernel_version,
            readable | string_type, "A" },
        NamedObject { "kern.maxvnodes", control_kernel, 5,
            readable | writable | integer_type },
        NamedObject { "kern.maxproc", control_kernel, 6 },
        NamedObject { "kern.maxfiles", control_kernel, 7 },
        NamedObject { "kern.argmax", control_kernel, 8 },
        NamedObject { "kern.hostname", control_kernel, 10,
            readable | writable | string_type, "A" },
        NamedObject { "kern.netboot", control_kernel, 40 },
        NamedObject { "kern.osversion", control_kernel, kernel_build_version,
            readable | string_type, "A" },
        NamedObject { "hw.machine", control_hardware, hardware_machine,
            readable | string_type, "A" },
        NamedObject { "hw.model", control_hardware, hardware_model,
            readable | string_type, "A" },
        NamedObject { "hw.ncpu", control_hardware, 3 },
        NamedObject { "hw.byteorder", control_hardware, 4 },
        NamedObject { "hw.physmem", control_hardware, 5 },
        NamedObject { "hw.usermem", control_hardware, 6 },
        NamedObject { "hw.pagesize", control_hardware, 7 },
        NamedObject { "hw.floatingpoint", control_hardware, 11 },
        NamedObject { "hw.vectorunit", control_hardware, 13 },
        NamedObject { "hw.memsize", control_hardware, 24,
            readable | quad_type, "Q" },
        NamedObject { "hw.availcpu", control_hardware, 25 },
    };

} // namespace

std::optional<ObjectMetadata> describe_object(
    std::uint32_t control, std::uint32_t selector)
{
    for (const auto& object : named_objects) {
        if (object.control == control && object.selector == selector)
            return ObjectMetadata { object.name, object.kind, object.format };
    }
    return std::nullopt;
}

std::vector<std::byte> encode_object_format(const ObjectMetadata& metadata)
{
    std::vector<std::byte> result(sizeof(metadata.kind) +
                                metadata.format.size() + 1U);
    for (std::size_t index = 0; index < sizeof(metadata.kind); ++index)
        result[index] = static_cast<std::byte>(metadata.kind >> (index * 8U));
    for (std::size_t index = 0; index < metadata.format.size(); ++index)
        result[sizeof(metadata.kind) + index] =
            static_cast<std::byte>(metadata.format[index]);
    return result;
}

std::optional<ObjectIdentifier> resolve_name(std::string_view name)
{
    if (name.ends_with('.')) {
        name.remove_suffix(1);
    }
    for (const auto& object : named_objects) {
        if (object.name == name) {
            return ObjectIdentifier { { object.control, object.selector }, 2 };
        }
    }
    return std::nullopt;
}

} // namespace ilemu::darwin::sysctl
