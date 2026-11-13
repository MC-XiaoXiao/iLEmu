/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "kernel/credential_groups.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ilemu {

struct PersonaInfo {
    std::uint32_t version { 1U };
    std::uint32_t id { UINT32_MAX };
    std::uint32_t type { };
    std::uint32_t gid { };
    std::uint32_t group_count { };
    std::array<std::uint32_t, CredentialGroups::maximum_count> groups { };
    std::uint32_t membership_uid { UINT32_MAX };
    std::array<char, 256> name { };
};
static_assert(sizeof(PersonaInfo) == 344U);

struct PersonaCredentials {
    std::shared_ptr<const PersonaInfo> identity;
    std::uint32_t uid;
    std::uint32_t gid;
    std::uint32_t effective_gid;
    CredentialGroups groups;
    std::string login;
};

// XNU kern_persona.c: identity allocation is separate from adopting an
// identity into a process. Callers hold KernelSharedState::mach_mutex.
class PersonaRegistry {
public:
    struct Allocation { std::uint32_t id; std::uint32_t error; };
    PersonaRegistry();
    [[nodiscard]] Allocation allocate(PersonaInfo info);
    bool remove(std::uint32_t id);
    [[nodiscard]] std::shared_ptr<const PersonaInfo> lookup(std::uint32_t id) const;
    [[nodiscard]] std::vector<std::uint32_t> find(const PersonaInfo& query) const;

private:
    std::map<std::uint32_t, std::shared_ptr<const PersonaInfo>> identities_;
    std::uint32_t next_id_ { 501U };
};

} // namespace ilemu
