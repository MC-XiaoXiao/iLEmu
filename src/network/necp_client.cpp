/* SPDX-License-Identifier: MPL-2.0 */
#include "network/necp_client.hpp"

#include <algorithm>
#include <utility>

namespace ilemu::darwin::necp {

Clients::Session::Session(std::shared_ptr<Clients> registry, bool observer)
    : registry_ { std::move(registry) }, observer_ { observer }
{
}

Clients::Session::~Session()
{
    std::lock_guard lock { registry_->mutex_ };
    for (const auto& [id, client] : clients_)
        registry_->clients_.erase(id);
}

std::shared_ptr<Clients::Session> Clients::open(bool observer)
{
    return std::shared_ptr<Session> { new Session { shared_from_this(), observer } };
}

void Clients::Session::add(ClientId id, Bytes parameters, Bytes result)
{
    std::lock_guard lock { registry_->mutex_ };
    const auto entry = clients_.emplace(id,
        Client { id, std::move(parameters), std::move(result) }).first;
    entry->second.generation = ++registry_->generation_;
    registry_->clients_.emplace(id, &entry->second);
}

void Clients::Session::remove(const ClientId& id)
{
    std::lock_guard lock { registry_->mutex_ };
    if (clients_.erase(id) != 0U)
        registry_->clients_.erase(id);
}

std::optional<bool> Clients::Session::copy(
    const ClientId& id, bool parameters, const Copier& copyout)
{
    std::lock_guard lock { registry_->mutex_ };
    auto found = clients_.find(id);
    const bool next_result = !parameters && id == ClientId { };
    if (next_result)
        found = std::find_if(clients_.begin(), clients_.end(),
            [](const auto& entry) { return entry.second.unread; });
    if (found != clients_.end()) {
        auto& client = found->second;
        const auto copied = copyout(parameters ? client.parameters : client.result);
        if (copied && !parameters)
            client.unread = false;
        return copied;
    }
    if (observer_ && !next_result) {
        const auto observed = registry_->clients_.find(id);
        if (observed != registry_->clients_.end())
            return copyout(parameters ? observed->second->parameters : observed->second->result);
    }
    return std::nullopt;
}

std::vector<ClientId> Clients::Session::list() const
{
    std::lock_guard lock { registry_->mutex_ };
    std::vector<ClientId> result;
    result.reserve(registry_->clients_.size());
    for (const auto& [id, client] : registry_->clients_)
        result.push_back(id);
    return result;
}

bool Clients::Session::readable() const
{
    std::lock_guard lock { registry_->mutex_ };
    return std::any_of(clients_.begin(), clients_.end(),
        [](const auto& entry) { return entry.second.unread; });
}

std::uint64_t Clients::Session::generation() const
{
    std::lock_guard lock { registry_->mutex_ };
    std::uint64_t result { };
    for (const auto& [id, client] : clients_)
        result = std::max(result, client.generation);
    return result;
}

bool Clients::refresh(const Evaluator& evaluate)
{
    std::lock_guard lock { mutex_ };
    bool changed { };
    for (auto& [id, client] : clients_) {
        auto result = evaluate(id, client->parameters);
        if (result != client->result) {
            client->result = std::move(result);
            client->unread = true;
            client->generation = ++generation_;
            changed = true;
        }
    }
    return changed;
}

} // namespace ilemu::darwin::necp
