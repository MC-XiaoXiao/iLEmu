#include "sandbox.hpp"

#include "ilemu/address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ilemu::bsd::sandbox {
namespace {

    constexpr std::uint32_t check_operation = 2U;
    constexpr std::uint32_t path_filter = 1U;
    constexpr std::size_t request_word_count = 6U;
    constexpr std::size_t maximum_operation_size = 256U;
    constexpr std::size_t maximum_path_size = 4096U;

} // namespace

CallResult dispatch(
    AddressSpace& memory, std::uint32_t operation, std::uint32_t argument)
{
    if (operation != check_operation)
        return CallResult::Unsupported;
    constexpr auto last_request_word_offset = static_cast<std::uint32_t>(
        (request_word_count - 1U) * sizeof(std::uint32_t));
    if (argument == 0U ||
        argument >
            std::numeric_limits<std::uint32_t>::max() -
                last_request_word_offset) {
        return CallResult::BadAddress;
    }

    std::array<std::uint32_t, request_word_count> request { };
    for (std::size_t index = 0; index < request.size(); ++index) {
        const auto value = memory.read32(
            argument +
            static_cast<std::uint32_t>(index * sizeof(std::uint32_t)));
        if (!value)
            return CallResult::BadAddress;
        request[index] = *value;
    }

    const auto decision_address = request[0];
    const auto operation_name_address = request[2];
    const auto filter_kind = request[3];
    const auto filter_address = request[4];
    if (!memory.read_c_string(operation_name_address, maximum_operation_size))
        return CallResult::BadAddress;
    if (filter_kind == path_filter &&
        !memory.read_c_string(filter_address, maximum_path_size)) {
        return CallResult::BadAddress;
    }

    // The compatibility kernel does not install or enforce a Sandbox MAC
    // policy. A check therefore has no policy denial to report. Returning
    // ENOSYS here makes libsandbox treat that absence as a denied operation.
    constexpr std::array<std::uint32_t, 2> allowed { 0U, 0U };
    if (!memory.copy_in(decision_address, std::as_bytes(std::span { allowed })))
        return CallResult::BadAddress;
    return CallResult::Success;
}

} // namespace ilemu::bsd::sandbox
