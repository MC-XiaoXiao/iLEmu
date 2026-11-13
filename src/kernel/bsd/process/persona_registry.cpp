/* SPDX-License-Identifier: MPL-2.0 */
#include "kernel/persona_registry.hpp"
#include "foundation/darwin_errno.hpp"
#include <algorithm>
#include <string_view>

namespace ilemu {

PersonaRegistry::PersonaRegistry()
{
    PersonaInfo system;
    system.id = 99U;
    system.type = 4U;
    system.gid = 99U;
    system.group_count = 1U;
    system.groups[0] = 99U;
    std::copy_n("system", 7U, system.name.begin());
    identities_.emplace(system.id, std::make_shared<PersonaInfo>(system));
}

PersonaRegistry::Allocation PersonaRegistry::allocate(PersonaInfo info)
{
    if (info.name[0] == '\0' || info.type == 0U || info.type > 4U ||
        info.group_count > CredentialGroups::maximum_count)
        return { 0U, darwin::error::invalid_argument };
    // The 512-identity limit and ten-ID allocation stride are XNU's ABI.
    if (identities_.size() == 512U)
        return { 0U, darwin::error::device_busy };
    for (const auto& [id, existing] : identities_) {
        if (std::string_view { existing->name.data() } == info.name.data())
            return { 0U, darwin::error::file_exists };
    }
    if (info.id == 0U || info.id == UINT32_MAX) {
        while (identities_.contains(next_id_))
            next_id_ += 10U;
        info.id = next_id_;
        next_id_ += 10U;
    } else if (identities_.contains(info.id)) {
        return { 0U, darwin::error::file_exists };
    }
    if (info.gid == 0U)
        info.gid = info.id;
    if (info.group_count == 0U) {
        info.group_count = 1U;
        info.groups[0] = info.gid;
        info.membership_uid = UINT32_MAX;
    } else if (info.membership_uid == 0U) {
        info.membership_uid = UINT32_MAX;
    }
    info.gid = info.groups[0];
    std::fill(info.groups.begin() + info.group_count, info.groups.end(), 0U);
    std::fill(std::find(info.name.begin(), info.name.end(), '\0'), info.name.end(), '\0');
    const auto id = info.id;
    identities_.emplace(id, std::make_shared<PersonaInfo>(std::move(info)));
    return { id, 0U };
}

bool PersonaRegistry::remove(std::uint32_t id)
{
    return identities_.erase(id) != 0U;
}

std::shared_ptr<const PersonaInfo> PersonaRegistry::lookup(std::uint32_t id) const
{
    const auto found = identities_.find(id);
    return found == identities_.end() ? nullptr : found->second;
}

std::vector<std::uint32_t> PersonaRegistry::find(const PersonaInfo& query) const
{
    std::vector<std::uint32_t> result;
    for (const auto& [id, info] : identities_) {
        if ((query.id == UINT32_MAX || query.id == id) &&
            (query.name[0] == '\0' ||
                std::string_view { query.name.data() } == info->name.data()))
            result.push_back(id);
    }
    return result;
}

} // namespace ilemu
