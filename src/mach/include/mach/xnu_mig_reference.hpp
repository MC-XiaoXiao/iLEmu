// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace ilemu::xnu::mig::reference {
// Public ARM32 reference evidence, not a runtime implementation claim.
// Firmware-derived contracts must take precedence over these desktop sources.
struct Field {
    std::string_view name, kind, type, c_type, count_field;
    std::string_view sent_disposition, received_disposition;
    std::uint32_t offset, minimum_offset, size, count, bits, descriptors;
    bool variable;
};
struct Layout {
    bool complex;
    std::uint32_t descriptors, size, minimum_size;
    std::span<const Field> fields;
};
struct Routine {
    std::uint32_t identifier, reply_identifier; // zero: one-way routine
    std::string_view name;
    const Layout* request;
    const Layout* reply; // null: no reply, including no RetCode
};
struct Subsystem {
    std::string_view name;
    std::uint32_t base, reference_end;
    // Zero is a skipped slot; nonzero indexes the shared routine pool plus1.
    std::span<const std::uint16_t> slots;
};
struct Profile {
    std::string_view name, source;
    std::span<const Subsystem> subsystems;
};
enum class Presence { UnknownProfile, OutsideCore, Absent, Present };
struct Resolution {
    Presence presence;
    const Subsystem* subsystem { };
    const Routine* routine { };
};
class Catalog {
public:
    explicit Catalog(std::string_view profile);
    [[nodiscard]] const Profile* profile() const { return profile_; }
    [[nodiscard]] Resolution resolve(std::uint32_t identifier) const;
    [[nodiscard]] bool valid() const;

private:
    const Profile* profile_ { };
};
} // namespace ilemu::xnu::mig::reference
