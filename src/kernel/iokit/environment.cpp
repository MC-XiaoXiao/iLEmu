// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Publish profile-driven environmental sensor registry services.

#include "environment.hpp"

#include "kernel/iokit_abi.hpp"
#include "kernel/kernel_shared_state.hpp"

#ifdef ILEMU_HAS_LIBPLIST
#include <plist/plist.h>
#endif

#include <memory>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ilemu::kernel_iokit::environment {
namespace {

    constexpr std::string_view io_service_class { "IOService" };
#ifdef ILEMU_HAS_LIBPLIST
    using Plist = std::unique_ptr<void, decltype(&plist_free)>;
    Plist parse(std::span<const std::byte> data)
    {
        const auto text =
            std::string_view { reinterpret_cast<const char*>(data.data()),
                data.size() };
        const auto xml = std::string { "<plist version=\"1.0\">" } +
                         std::string { text.substr(0, text.find('\0')) } +
                         "</plist>";
        plist_t parsed = nullptr;
        plist_from_xml(
            xml.data(), static_cast<std::uint32_t>(xml.size()), &parsed);
        return { parsed, plist_free };
    }
#endif

    bool contains(std::span<const std::byte> bytes, std::string_view value)
    {
        return std::search(bytes.begin(), bytes.end(), value.begin(),
                   value.end(), [](std::byte byte, char character) {
                       return std::to_integer<unsigned char>(byte) ==
                              static_cast<unsigned char>(character);
                   }) != bytes.end();
    }

    std::vector<std::byte> bytes_from_string(std::string_view value)
    {
        std::vector<std::byte> bytes(value.size());
        std::transform(value.begin(), value.end(), bytes.begin(),
            [](char character) { return static_cast<std::byte>(character); });
        return bytes;
    }

    KernelSharedState::IOKitRegistryProperty string_property(
        std::string_view value)
    {
        return { KernelSharedState::IOKitRegistryProperty::Kind::String,
            bytes_from_string(value) };
    }

    KernelSharedState::IOKitRegistryProperty number_property(
        std::uint32_t value)
    {
        std::vector<std::byte> bytes(sizeof(value));
        for (std::size_t index = 0; index < bytes.size(); ++index)
            bytes[index] = static_cast<std::byte>(value >> (index * 8U));
        return { KernelSharedState::IOKitRegistryProperty::Kind::Number,
            std::move(bytes) };
    }

} // namespace

bool matches_service(
    std::span<const std::byte> matching, const KernelSharedState& state)
{
    return !state.ambient_light_sensor.service_class.empty() &&
           contains(matching, state.ambient_light_sensor.service_class);
}

std::uint32_t ensure_service_locked(KernelSharedState& state)
{
    const auto service_class = state.ambient_light_sensor.service_class;
    const auto existing = std::find_if(state.iokit_services.begin(),
        state.iokit_services.end(), [service_class](const auto& entry) {
            return entry.second.class_name == service_class;
        });
    if (existing != state.iokit_services.end())
        return existing->first;

    const auto object = state.allocate_mach_object();
    static_cast<void>(state.mach_port_objects.create(object));
    state.mach_queues.try_emplace(object);

    std::map<std::string, KernelSharedState::IOKitRegistryProperty> properties;
    properties.emplace("Product", string_property("ambient"));
    properties.emplace("ALSCh0Gain",
        number_property(state.ambient_light_sensor.channel0_gain));
    properties.emplace("ALSCh1Gain",
        number_property(state.ambient_light_sensor.channel1_gain));
    properties.emplace("ALSIntegrationCycles",
        number_property(state.ambient_light_sensor.integration_cycles));
    properties.emplace("PrimaryUsagePage", number_property(0x01U));
    properties.emplace("PrimaryUsage", number_property(0x04U));

    std::string registry_path { "IOService:/" };
    registry_path += service_class;
    state.iokit_services.emplace(object,
        KernelSharedState::IOKitService { std::string { service_class },
            { std::string { io_service_class } }, std::move(properties),
            std::move(registry_path), state.iokit_registry_root_object });
    return object;
}

bool matches_hid_service(std::span<const std::byte> matching)
{
#ifdef ILEMU_HAS_LIBPLIST
    const auto root = parse(matching);
    const auto properties = plist_dict_get_item(root.get(), "IOPropertyMatch");
    if (!properties)
        return false;
    std::uint64_t page = 0, usage = 0;
    plist_get_uint_val(
        plist_dict_get_item(properties, "PrimaryUsagePage"), &page);
    plist_get_uint_val(plist_dict_get_item(properties, "PrimaryUsage"), &usage);
    return page == 0xff00U && usage == 4U;
#else
    return false;
#endif
}

std::uint32_t ensure_hid_service_locked(KernelSharedState& state)
{
    if (state.ambient_light_hid_service != 0U)
        return state.ambient_light_hid_service;
    const auto object = state.allocate_mach_object();
    state.ambient_light_hid_service = object;
    static_cast<void>(state.mach_port_objects.create(object));
    state.mach_queues.try_emplace(object);

    using Property = KernelSharedState::IOKitRegistryProperty;
    Property plugins;
    plugins.kind = Property::Kind::Dictionary;
    plugins.dictionary_value.emplace("0516B563-B15B-11DA-96EB-0014519758EF",
        string_property("IOHIDFamily.kext/PlugIns/IOHIDLib.plugin"));
    std::map<std::string, Property> properties;
    properties.emplace("HIDServiceSupport",
        Property { Property::Kind::Boolean, { std::byte { 1 } } });
    // The sensor reports calibrated lux; firmware's adaptive plugin consumes
    // that format instead of the legacy raw photodiode channels.
    properties.emplace("UseAABPlugin",
        Property { Property::Kind::Boolean, { std::byte { 1 } } });
    properties.emplace("IOProviderClass", string_property("IOService"));
    properties.emplace(
        "Product", string_property("Virtual Ambient Light Sensor"));
    properties.emplace("PrimaryUsagePage", number_property(0xff00U));
    properties.emplace("PrimaryUsage", number_property(4U));
    Property usage;
    usage.kind = Property::Kind::Dictionary;
    usage.dictionary_value.emplace("DeviceUsagePage", number_property(0xff00U));
    usage.dictionary_value.emplace("DeviceUsage", number_property(4U));
    Property usages;
    usages.kind = Property::Kind::Array;
    usages.array_value.push_back(std::move(usage));
    properties.emplace("DeviceUsagePairs", std::move(usages));
    properties.emplace("IOCFPlugInTypes", std::move(plugins));
    state.iokit_services.emplace(object,
        KernelSharedState::IOKitService { "IOHIDEventService", { "IOService" },
            std::move(properties), "IOService:/AmbientLightSensor",
            state.iokit_registry_root_object,
            KernelSharedState::IOKitUserClientKind::AmbientLightHid });
    return object;
}

std::uint32_t set_hid_properties_locked(KernelSharedState& state,
    KernelSharedState::IOKitService& service, std::span<const std::byte> data)
{
#ifdef ILEMU_HAS_LIBPLIST
    const auto properties = parse(data);
    for (const auto* key : { "IOHIDALSContinuousMode", "ReportInterval",
             "ReportMode", "IOHIDALSTestMode", "IOHIDBacklightLevel" }) {
        const auto item = plist_dict_get_item(properties.get(), key);
        if (!item)
            continue;
        std::uint64_t value = 0;
        plist_get_uint_val(item, &value);
        service.properties[key] =
            number_property(static_cast<std::uint32_t>(value));
        if (std::string_view { key } == "IOHIDALSContinuousMode")
            state.ambient_light_input.set_continuous(
                value != 0U, state.clock.now());
        else if (std::string_view { key } == "ReportInterval")
            state.ambient_light_input.set_report_interval(
                static_cast<std::uint32_t>(value), state.clock.now());
    }
    state.note_kernel_event_transition();
    return iokit_abi::success;
#else
    return iokit_abi::unsupported;
#endif
}

} // namespace ilemu::kernel_iokit::environment
