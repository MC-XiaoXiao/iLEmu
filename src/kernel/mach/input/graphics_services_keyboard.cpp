// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/graphics_services_keyboard.hpp"
#include "foundation/macho.hpp"
#include "kernel/graphics_services_input_abi.hpp"

#include <codecvt>
#include <cstdint>
#include <locale>
#include <stdexcept>

namespace ilemu {
std::optional<std::vector<GraphicsServicesKeyboard::Payload>>
GraphicsServicesKeyboard::encode(
    std::string_view utf8, KeyboardWireProfile profile)
{
    // Bound one host submission before conversion/allocation. A malformed
    // submission is rejected atomically, never partially typed.
    if (profile == KeyboardWireProfile::Unavailable || utf8.empty() ||
        utf8.size() > 64U * 1024U)
        return std::nullopt;
    std::u16string text;
    try {
        std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>
            converter;
        text = converter.from_bytes(utf8.data(), utf8.data() + utf8.size());
        if (converter.converted() != utf8.size())
            return std::nullopt;
    } catch (const std::range_error&) {
        return std::nullopt;
    }
    std::vector<Payload> result;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = text[offset];
        const auto count = first >= 0xd800 && first <= 0xdbff ? 2U : 1U;
        if ((count == 2U &&
                (offset + 1U >= text.size() || text[offset + 1U] < 0xdc00 ||
                    text[offset + 1U] > 0xdfff)) ||
            (first >= 0xdc00 && first <= 0xdfff) || first == 0)
            return std::nullopt;
        const auto extended = profile == KeyboardWireProfile::Utf16Extended;
        Payload info(extended ? 162U : 10U, std::byte { 0 });
        const auto half = [&info](std::size_t at, std::uint16_t value) {
            info[at] = static_cast<std::byte>(value);
            info[at + 1U] = static_cast<std::byte>(value >> 8U);
        };
        if (!extended) {
            // Scalar key records carry one UTF16 unit per event. Keep a
            // surrogate pair adjacent; firmware owns decoding/editing.
            for (std::size_t i = 0; i < count; ++i) {
                half(0U, text[offset + i]);
                half(2U, text[offset + i]);
                half(4U, text[offset + i]);
                result.push_back(info);
            }
            offset += count;
            continue;
        }
        half(8U, count);
        half(58U, count);
        for (std::size_t i = 0; i < count; ++i) {
            half(10U + i * 2U, text[offset + i]);
            half(60U + i * 2U, text[offset + i]);
        }
        // The host has already committed its IME composition.
        half(154U, 4U);
        result.push_back(std::move(info));
        offset += count;
    }
    return result;
}
KeyboardWireProfile GraphicsServicesKeyboard::detect(const MachOImage& image)
{
    const auto size = GraphicsServicesInputAbi::detect_record_size(image);
    if (!image.find_symbol("_GSEventCopyCharacters"))
        return KeyboardWireProfile::Unavailable;
    if (size == 48U)
        return KeyboardWireProfile::ScalarCompact;
    if (size == 52U)
        return image.find_symbol("_GSEventSetCharacters")
                   ? KeyboardWireProfile::Utf16Extended
                   : KeyboardWireProfile::ScalarExtended;
    return KeyboardWireProfile::Unavailable;
}
} // namespace ilemu
