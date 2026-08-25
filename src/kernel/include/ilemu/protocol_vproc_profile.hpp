#pragma once

#include "ilemu/protocol_vproc_mig_ids.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace ilemu::protocol_vproc {

// Apple kept protocol_vproc's public prefix stable while adding and removing
// private routines in the tail.  Describe the wire capability that changes
// the following routine identifiers rather than keying compatibility to an
// OS build or to a particular executable path.
struct Profile {
    std::string_view name;
    std::uint32_t log_message_id;
};

inline constexpr Profile without_service_policy { "without-service-policy",
    xnu792::mig::protocol_vproc::id(
        xnu792::mig::protocol_vproc::Routine::swap_integer) +
        1U };
inline constexpr Profile with_service_policy { "with-service-policy",
    xnu792::mig::protocol_vproc::id(
        xnu792::mig::protocol_vproc::Routine::log) };
inline constexpr std::array profiles { without_service_policy,
    with_service_policy };

[[nodiscard]] constexpr const Profile* profile_for_log_message(
    std::uint32_t message_id)
{
    for (const auto& profile : profiles) {
        if (profile.log_message_id == message_id)
            return &profile;
    }
    return nullptr;
}

} // namespace ilemu::protocol_vproc
