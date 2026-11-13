/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace ilemu::darwin::necp {

using ClientId = std::array<std::byte, 16>;
using Bytes = std::vector<std::byte>;

// XNU3789 necp_client.c: each open description owns its clients and unread
// policy results. Observers can query other descriptions without draining them.
class Clients : public std::enable_shared_from_this<Clients> {
    struct Client {
        ClientId id;
        Bytes parameters;
        Bytes result;
        bool unread { true };
        std::uint64_t generation { };
    };

public:
    using Evaluator = std::function<Bytes(const ClientId&, std::span<const std::byte>)>;
    using Copier = std::function<bool(std::span<const std::byte>)>;

    class Session {
    public:
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        void add(ClientId id, Bytes parameters, Bytes result);
        void remove(const ClientId& id);
        [[nodiscard]] std::optional<bool> copy(
            const ClientId& id, bool parameters, const Copier& copyout);
        [[nodiscard]] std::vector<ClientId> list() const;
        [[nodiscard]] bool readable() const;
        [[nodiscard]] std::uint64_t generation() const;
        [[nodiscard]] bool observer() const { return observer_; }

    private:
        friend class Clients;
        Session(std::shared_ptr<Clients> registry, bool observer);
        std::shared_ptr<Clients> registry_;
        bool observer_;
        std::map<ClientId, Client> clients_;
    };

    [[nodiscard]] std::shared_ptr<Session> open(bool observer);
    bool refresh(const Evaluator& evaluate);

private:
    mutable std::mutex mutex_;
    // Session node addresses remain stable; its destructor unregisters them
    // under this mutex before destroying the owning map.
    std::map<ClientId, Client*> clients_;
    std::uint64_t generation_ { };
};

} // namespace ilemu::darwin::necp
