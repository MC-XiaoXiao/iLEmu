/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace ilemu::nexus {

using Identifier = std::array<std::byte, 16>;
using ProviderParameters = std::array<std::byte, 116>;
struct ProviderEntry {
    Identifier identifier;
    ProviderParameters parameters;
};

// The compact provider ABI used by the quantum-based channel interface.
struct CompactProviderRegistration {
    static constexpr std::size_t size = 124;
    static constexpr std::size_t parameters_offset = 8;
    [[nodiscard]] static std::uint32_t validate(std::span<const std::byte> registration);
    [[nodiscard]] static std::uint32_t prepare(
        std::span<const std::byte> registration, ProviderParameters& parameters);
};

struct PortBinding {
    std::uint32_t flags { };
    std::uint32_t process { };
    std::uint64_t process_incarnation { };
    Identifier executable { };
    std::vector<std::byte> key;
};

struct Instance {
    Identifier identifier;
    bool closed { };
    bool anonymous { };
    std::vector<Identifier> channels;
    std::map<std::uint32_t, PortBinding> bindings;
};

struct Provider {
    Identifier identifier;
    Identifier controller;
    ProviderParameters parameters;
    bool closed { };
    std::map<Identifier, std::shared_ptr<Instance>> instances;
};

class Controller;
class Registry {
public:
    struct State {
        std::mutex mutex;
        std::map<Identifier, std::shared_ptr<Provider>> providers;
    };
    [[nodiscard]] std::shared_ptr<Controller> open(
        std::array<bool, 5> registration_privileges);

private:
    std::shared_ptr<State> state_ { std::make_shared<State>() };
};

class Controller {
public:
    Controller(std::shared_ptr<Registry::State> state,
        std::array<bool, 5> registration_privileges);
    ~Controller();
    [[nodiscard]] std::uint32_t register_provider(
        std::span<const std::byte> registration, Identifier& identifier);
    [[nodiscard]] std::uint32_t deregister_provider(const Identifier& identifier);
    [[nodiscard]] std::uint32_t create_instance(
        const Identifier& provider, Identifier& identifier);
    [[nodiscard]] std::uint32_t destroy_instance(const Identifier& identifier);
    [[nodiscard]] std::uint32_t provider_parameters(
        const Identifier& identifier, ProviderParameters& parameters) const;
    [[nodiscard]] std::uint32_t bind(const Identifier& instance,
        std::uint32_t port, PortBinding binding);
    [[nodiscard]] std::uint32_t unbind(
        const Identifier& instance, std::uint32_t port);
    [[nodiscard]] std::vector<ProviderEntry> providers() const;
    [[nodiscard]] std::uint32_t configure(const Identifier& instance) const;
    [[nodiscard]] std::uint32_t channel_identifiers(const Identifier& instance,
        std::vector<Identifier>& identifiers) const;
    [[nodiscard]] std::uint32_t instances(const Identifier& provider,
        std::vector<Identifier>& identifiers) const;

private:
    [[nodiscard]] std::shared_ptr<Provider> find_provider(
        const Identifier& identifier) const;
    [[nodiscard]] std::shared_ptr<Instance> find_instance(
        const Identifier& identifier, bool observe = false) const;
    std::shared_ptr<Registry::State> state_;
    Identifier identifier_;
    std::array<bool, 5> registration_privileges_;
};

} // namespace ilemu::nexus
