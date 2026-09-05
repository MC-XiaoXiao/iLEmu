#pragma once

#include "ilemu/address_space.hpp"
#include "ilemu/darwin_kernel_profile.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace ilemu::mach_vm_support {

// Later ARM32 mach_vm clients widened addresses and sizes within Darwin 11.
// The vm_map subsystem retains natural-sized fields. Both use four-byte MIG
// alignment, including for an eight-byte address following a scalar word.
class MachVmWireProfile {
public:
    [[nodiscard]] static constexpr MachVmWireProfile for_interface(
        bool mach_vm, DarwinMachVmAddressProfile address_profile)
    {
        const auto wide =
            mach_vm && address_profile == DarwinMachVmAddressProfile::Wide64;
        return MachVmWireProfile { wide ? 8U : 4U };
    }

    [[nodiscard]] constexpr std::uint32_t address_size() const { return size_; }

    [[nodiscard]] std::optional<std::uint64_t> read_address(
        const AddressSpace& memory, std::uint32_t address) const
    {
        if (size_ == 8U)
            return memory.read64(address);
        if (const auto value = memory.read32(address))
            return *value;
        return std::nullopt;
    }

    void append_address(
        std::vector<std::uint32_t>& words, std::uint64_t address) const
    {
        words.push_back(static_cast<std::uint32_t>(address));
        if (size_ == 8U)
            words.push_back(static_cast<std::uint32_t>(address >> 32U));
    }

private:
    explicit constexpr MachVmWireProfile(std::uint32_t size)
        : size_ { size }
    {
    }
    std::uint32_t size_;
};

} // namespace ilemu::mach_vm_support
