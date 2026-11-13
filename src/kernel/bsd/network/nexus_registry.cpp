/* SPDX-License-Identifier: MPL-2.0 */
// XNU nexus controller/provider ownership; compact ARM ABI from the firmware's
// os_nexus_controller_register_provider and nxprov_params_adjust implementations.
#include "kernel/nexus_registry.hpp"
#include "crypto/host_uuid.hpp"
#include "foundation/darwin_errno.hpp"

#include <algorithm>
#include <charconv>
#include <utility>

namespace ilemu::nexus {
namespace {

std::uint32_t word(std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint32_t result { };
    for (unsigned i = 0; i < 4; ++i)
        result |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (i * 8U);
    return result;
}

void put_word(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::byte>(value >> (i * 8U));
}

Identifier new_identifier()
{
    const auto text = make_host_uuid();
    auto cursor = text.data();
    Identifier result;
    for (auto& byte : result) {
        if (*cursor == '-')
            ++cursor;
        unsigned value;
        static_cast<void>(std::from_chars(cursor, cursor + 2, value, 16));
        byte = static_cast<std::byte>(value);
        cursor += 2;
    }
    return result;
}

void close_provider(Provider& provider)
{
    provider.closed = true;
    for (const auto& [_, instance] : provider.instances)
        instance->closed = true;
    provider.instances.clear();
}

} // namespace

std::uint32_t CompactProviderRegistration::validate(
    std::span<const std::byte> registration)
{
    return registration.size() >= size && word(registration, 0) == 1U &&
        word(registration, 72) != 0U && word(registration, 72) <= 64U
        ? 0U : darwin::error::invalid_argument;
}

std::uint32_t CompactProviderRegistration::prepare(
    std::span<const std::byte> registration, ProviderParameters& parameters)
{
    parameters.fill(std::byte { });
    std::copy_n(registration.begin() + parameters_offset, 72, parameters.begin());
    put_word(parameters, 76, word(registration, 84));
    const auto requested = word(registration, 4);
    struct Bounds { std::uint32_t value, minimum, maximum, flag; };
    // Native quantum user-pipe domain bounds; these constrain the ABI's shared
    // rings and slot layout, rather than imposing a host resource threshold.
    constexpr std::array bounds {
        Bounds { 1, 1, 16, 1U }, Bounds { 1, 1, 16, 2U },
        Bounds { 64, 2, 4096, 4U }, Bounds { 64, 2, 4096, 8U },
        Bounds { 2048, 1024, 16384, 16U }, Bounds { 0, 0, 448, 32U },
        Bounds { 0, 0, 64, 128U }, Bounds { 0, 0, 0, 256U },
        Bounds { 0, 0, 7, 512U }
    };
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const auto& bound = bounds[i];
        const auto offset = 80U + i * 4U;
        auto value = (requested & bound.flag) != 0U
            ? word(registration, parameters_offset + offset) : bound.value;
        if (value < bound.minimum)
            value = bound.value;
        if (value > bound.maximum)
            return darwin::error::no_memory;
        put_word(parameters, offset, value);
    }
    if (word(parameters, 80) != word(parameters, 84))
        return darwin::error::invalid_argument;
    if ((requested & 64U) != 0U)
        put_word(parameters, 72, 1U);
    return 0U;
}

std::shared_ptr<Controller> Registry::open(std::array<bool, 5> privileges)
{
    return std::make_shared<Controller>(state_, privileges);
}

Controller::Controller(std::shared_ptr<Registry::State> state,
    std::array<bool, 5> privileges)
    : state_ { std::move(state) }, identifier_ { new_identifier() },
      registration_privileges_ { privileges }
{
}

Controller::~Controller()
{
    const std::lock_guard lock { state_->mutex };
    for (auto it = state_->providers.begin(); it != state_->providers.end();) {
        if (it->second->controller == identifier_) {
            close_provider(*it->second);
            it = state_->providers.erase(it);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<Provider> Controller::find_provider(const Identifier& id) const
{
    const auto found = state_->providers.find(id);
    return found != state_->providers.end() && found->second->controller == identifier_
        ? found->second : nullptr;
}

std::shared_ptr<Instance> Controller::find_instance(const Identifier& id, bool observe) const
{
    for (const auto& [_, provider] : state_->providers) {
        if (provider->controller != identifier_ && !(observe && registration_privileges_[4]))
            continue;
        const auto found = provider->instances.find(id);
        if (found != provider->instances.end())
            return found->second;
    }
    return nullptr;
}

std::uint32_t Controller::register_provider(
    std::span<const std::byte> registration, Identifier& id)
{
    const auto type = word(registration, 76);
    if (type != 0U && type != 2U && type != 3U)
        return darwin::error::invalid_argument;
    if (!registration_privileges_[type])
        return darwin::error::operation_not_permitted;
    // Net-if and flow-switch domains need a registered kernel domain provider.
    // Network sockets do not constitute such a provider.
    if (type != 0U)
        return darwin::error::no_such_device_or_address;
    ProviderParameters parameters;
    const auto error = CompactProviderRegistration::prepare(registration, parameters);
    if (error != 0U)
        return error;
    id = new_identifier();
    auto provider = std::make_shared<Provider>();
    provider->identifier = id;
    provider->controller = identifier_;
    provider->parameters = parameters;
    const std::lock_guard lock { state_->mutex };
    state_->providers.emplace(id, std::move(provider));
    return 0U;
}

std::uint32_t Controller::deregister_provider(const Identifier& id)
{
    const std::lock_guard lock { state_->mutex };
    const auto provider = find_provider(id);
    if (!provider)
        return darwin::error::no_entry;
    close_provider(*provider);
    state_->providers.erase(id);
    return 0U;
}

std::uint32_t Controller::create_instance(const Identifier& provider_id, Identifier& id)
{
    const std::lock_guard lock { state_->mutex };
    const auto provider = find_provider(provider_id);
    if (!provider)
        return darwin::error::no_entry;
    id = new_identifier();
    auto instance = std::make_shared<Instance>();
    instance->identifier = id;
    instance->anonymous = (word(provider->parameters, 72) & 1U) != 0U;
    provider->instances.emplace(id, std::move(instance));
    return 0U;
}

std::uint32_t Controller::destroy_instance(const Identifier& id)
{
    const std::lock_guard lock { state_->mutex };
    for (const auto& [_, provider] : state_->providers) {
        if (provider->controller != identifier_)
            continue;
        const auto found = provider->instances.find(id);
        if (found == provider->instances.end())
            continue;
        found->second->closed = true;
        provider->instances.erase(found);
        return 0U;
    }
    return darwin::error::no_entry;
}

std::uint32_t Controller::provider_parameters(const Identifier& id,
    ProviderParameters& parameters) const
{
    const std::lock_guard lock { state_->mutex };
    const auto found = state_->providers.find(id);
    if (found == state_->providers.end() ||
        (found->second->controller != identifier_ && !registration_privileges_[4]))
        return darwin::error::no_entry;
    const auto& provider = found->second;
    parameters = provider->parameters;
    return 0U;
}

std::uint32_t Controller::bind(const Identifier& id, std::uint32_t port, PortBinding binding)
{
    const std::lock_guard lock { state_->mutex };
    const auto instance = find_instance(id);
    if (!instance)
        return darwin::error::no_entry;
    if (instance->anonymous)
        return darwin::error::no_such_device_or_address;
    if (port == 0xffffffffU)
        return darwin::error::operation_not_permitted;
    if (port > 1U)
        return 33U; // EDOM: not a user-pipe port.
    if (instance->bindings.contains(port))
        return darwin::error::file_exists;
    instance->bindings.emplace(port, std::move(binding));
    return 0U;
}

std::uint32_t Controller::unbind(const Identifier& id, std::uint32_t port)
{
    const std::lock_guard lock { state_->mutex };
    const auto instance = find_instance(id);
    if (!instance)
        return darwin::error::no_entry;
    if (instance->anonymous)
        return darwin::error::no_such_device_or_address;
    if (port == 0xffffffffU)
        return darwin::error::invalid_argument;
    if (port > 1U)
        return 33U; // EDOM: not a user-pipe port.
    return instance->bindings.erase(port) != 0U ? 0U : darwin::error::no_entry;
}

std::uint32_t Controller::channel_identifiers(const Identifier& id,
    std::vector<Identifier>& ids) const
{
    const std::lock_guard lock { state_->mutex };
    const auto instance = find_instance(id, true);
    if (!instance)
        return darwin::error::no_entry;
    ids = instance->channels;
    return 0U;
}

std::uint32_t Controller::configure(const Identifier& id) const
{
    const std::lock_guard lock { state_->mutex };
    return find_instance(id) ? darwin::error::operation_not_permitted
                            : darwin::error::no_entry;
}

std::vector<ProviderEntry> Controller::providers() const
{
    const std::lock_guard lock { state_->mutex };
    std::vector<ProviderEntry> entries;
    for (const auto& [id, provider] : state_->providers)
        if (provider->controller == identifier_ || registration_privileges_[4])
            entries.push_back({ id, provider->parameters });
    return entries;
}

std::uint32_t Controller::instances(const Identifier& provider_id,
    std::vector<Identifier>& ids) const
{
    const std::lock_guard lock { state_->mutex };
    const auto found = state_->providers.find(provider_id);
    if (found == state_->providers.end() ||
        (found->second->controller != identifier_ && !registration_privileges_[4]))
        return darwin::error::no_entry;
    for (const auto& [id, _] : found->second->instances)
        ids.push_back(id);
    return 0U;
}

} // namespace ilemu::nexus
