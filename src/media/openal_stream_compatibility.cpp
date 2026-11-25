// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "media/openal_stream_compatibility.hpp"

#include "foundation/cpu.hpp"
#include "foundation/macho.hpp"
#include "foundation/userland_hle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ilemu {

void register_openal_stream_compatibility(UserlandHleRegistry& registry)
{
    constexpr std::string_view app_path { "/GTRacing.app/GTRacing" };
    constexpr std::array uuid {
        std::byte { 0xf2 }, std::byte { 0xc8 }, std::byte { 0x00 },
        std::byte { 0xe0 }, std::byte { 0xbe }, std::byte { 0x40 },
        std::byte { 0xc4 }, std::byte { 0x3e }, std::byte { 0x5a },
        std::byte { 0x23 }, std::byte { 0xa1 }, std::byte { 0xbc },
        std::byte { 0xd1 }, std::byte { 0x1d }, std::byte { 0xc1 },
        std::byte { 0xba },
    };
    constexpr std::uint32_t upload = 0x000c1ef6U;
    constexpr std::uint32_t epilogue = 0x000c1f5aU;
    registry.register_address(std::string { app_path }, upload | 1U,
        "GTRacingOpenALStreamEof", [](UserlandHleCall& call) {
            const auto& registers = call.cpu().registers();
            if (registers[4] != 0U) {
                // The original ldrd uses r1 as its base and can execute in
                // the persistent Thumb trampoline without relocation.
                call.resume_original_persistently();
                return;
            }
            // A refill has already unqueued the processed buffer. At EOF,
            // alBufferData rejects the zero-byte upload, leaving old samples
            // that would play again if the app requeued that buffer.
            constexpr std::uint32_t thumb_svc_size = 2U;
            const auto entry = registers[15] - thumb_svc_size;
            call.resume_original_at((entry + epilogue - upload) | 1U);
        }, [uuid](const MachOImage& image) {
            return image.uuid() == uuid &&
                   image.read_vm_u32(upload) == 0x2303e9d1U &&
                   image.read_vm_u32(epilogue) == 0xb08546bdU;
        });
}

} // namespace ilemu
