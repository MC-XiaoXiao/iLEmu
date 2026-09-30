// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xnu_mig_reference_data.hpp"
#include <algorithm>
#include <limits>
#include <mach/xnu_mig_reference.hpp>
namespace ilemu::xnu::mig::reference {
Catalog::Catalog(std::string_view name)
{
    for (const auto& candidate : data::profiles) {
        if (candidate.name == name) {
            profile_ = &candidate;
            break;
        }
    }
}
Resolution Catalog::resolve(std::uint32_t identifier) const
{
    if (!profile_)
        return { Presence::UnknownProfile };
    for (const auto& subsystem : profile_->subsystems) {
        if (identifier < subsystem.base ||
            identifier - subsystem.base >= subsystem.slots.size())
            continue;
        const auto index = subsystem.slots[identifier - subsystem.base];
        if (!index)
            return { Presence::Absent, &subsystem };
        return { Presence::Present, &subsystem, &data::routines[index - 1U] };
    }
    return { Presence::OutsideCore };
}
namespace {
    bool valid_layout(const Layout& layout)
    {
        if (layout.minimum_size > layout.size || layout.minimum_size < 24U)
            return false;
        std::uint32_t descriptors = 0;
        for (const auto& field : layout.fields) {
            if (field.minimum_offset > field.offset ||
                field.offset > layout.size ||
                field.size > layout.size - field.offset)
                return false;
            descriptors += field.descriptors;
            if (field.variable) {
                if (field.bits == 0 || field.bits % 8U != 0 ||
                    std::uint64_t { field.count } * (field.bits / 8U) !=
                        field.size)
                    return false;
                const auto count = std::find_if(layout.fields.begin(),
                    layout.fields.end(), [&](const Field& candidate) {
                        return candidate.name == field.count_field;
                    });
                if (count == layout.fields.end() || count->kind != "count")
                    return false;
            }
        }
        return descriptors == layout.descriptors &&
               layout.complex == (descriptors != 0);
    }
}
bool Catalog::valid() const
{
    if (!profile_ || profile_->subsystems.size() != 10U)
        return false;
    for (const auto& sub : profile_->subsystems) {
        if (sub.slots.empty() ||
            sub.slots.size() >
                std::numeric_limits<std::uint32_t>::max() - sub.base)
            return false;
        if (sub.reference_end < sub.base ||
            sub.reference_end > sub.base + sub.slots.size())
            return false;
        for (const auto& other : profile_->subsystems) {
            if (&sub != &other && sub.base < other.base + other.slots.size() &&
                other.base < sub.base + sub.slots.size())
                return false;
        }
        for (std::size_t i = 0; i < sub.slots.size(); ++i) {
            const auto index = sub.slots[i];
            if (!index)
                continue;
            if (index > data::routines.size() ||
                sub.base + i >= sub.reference_end)
                return false;
            const auto& r = data::routines[index - 1U];
            if (r.identifier != sub.base + i || r.name.empty() || !r.request ||
                !valid_layout(*r.request) ||
                (r.reply == nullptr) != (r.reply_identifier == 0))
                return false;
            if (r.reply && (r.reply_identifier != r.identifier + 100U ||
                               !valid_layout(*r.reply)))
                return false;
        }
    }
    return true;
}
} // namespace ilemu::xnu::mig::reference
