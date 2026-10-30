// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/display_console.hpp"

#include "console_progress_image.hpp"
#include "foundation/darwin_version.hpp"

#include <algorithm>
#include <limits>
#include <span>

namespace ilemu {

struct ConsoleProgressImageProfile {
    std::uint32_t frames;
    std::span<const unsigned char> image_1x;
    std::span<const unsigned char> image_2x;
    bool rotate_on_wrap;
};

// XNU pexpert/pe_images.h and osfmk/console/video_console.c: the default
// alpha gear runs at 1000 / kGearFPS milliseconds per frame. The ARM screen
// release path resets dx/dy to zero, so reacquiring centers it on the panel.
// GearImage.bin is the unchanged gGearPict from XNU-1504.3.12
// pexpert/pexpert/GearImage.h (APSL-2.0; external/xnu-console/APPLE_LICENSE).
// It is byte-identical to the progress image in the examined native kernel.
namespace {
    constexpr std::uint32_t gear_width = 32;
    constexpr std::uint32_t gear_height = 32;
    constexpr std::uint32_t gear_frames = 24;
    constexpr std::uint64_t gear_interval = (1000U / 24U) * 1'000'000ULL;
    static_assert(sizeof(console_progress_image) ==
                  gear_width * gear_height * gear_frames);
    constexpr ConsoleProgressImageProfile scaled_alpha_gear { gear_frames,
        console_progress_image, { }, false };
    // XNU-2050's pexpert supplies six samples and an authored Retina bitmap.
    // Keep resource selection separate from console ownership and scheduling.
    constexpr ConsoleProgressImageProfile native_scale_alpha_gear { 6,
        console_progress_native_1x, console_progress_native_2x, true };
    static_assert(
        sizeof(console_progress_native_1x) == gear_width * gear_height * 6);
    static_assert(
        sizeof(console_progress_native_2x) == gear_width * gear_height * 6 * 4);
}

void DisplayConsole::configure(DarwinVersion version)
{
    profile_ = version >= DarwinGeneration::Ios6 ? &native_scale_alpha_gear
                                                 : &scaled_alpha_gear;
}

void DisplayConsole::acquire(std::uint64_t now)
{
    if (!clients_.empty() || deadline_)
        return;
    started_at_ = now;
    deadline_ = now;
}

void DisplayConsole::open(std::uint32_t connection)
{
    clients_.insert(connection);
    deadline_.reset(); // kPEReleaseScreen cancels the progress callout.
}

void DisplayConsole::close(std::uint32_t connection, std::uint64_t now)
{
    if (clients_.erase(connection) == 0 || !clients_.empty())
        return;
    acquire(now); // kPEAcquireScreen uses a zero-delay callout on ARM.
}

std::optional<std::uint64_t> DisplayConsole::next_deadline() const
{
    return deadline_;
}

std::optional<std::vector<std::uint32_t>> DisplayConsole::take_frame(
    std::uint64_t now, DisplayGeometry panel, DisplayGeometry logical,
    bool powered_on)
{
    if (!deadline_ || now < *deadline_ || !panel.valid())
        return std::nullopt;
    const auto elapsed = now - started_at_;
    const auto until_next = gear_interval - elapsed % gear_interval;
    deadline_ = now > std::numeric_limits<std::uint64_t>::max() - until_next
                    ? std::numeric_limits<std::uint64_t>::max()
                    : now + until_next;
    if (!powered_on)
        return std::nullopt;
    // pexpert expands the kernel bitmap by the panel's integral pixel scale.
    const auto scale = logical.valid()
                           ? std::max(1U, std::min(panel.width / logical.width,
                                              panel.height / logical.height))
                           : 1U;
    const auto width = gear_width * scale;
    const auto height = gear_height * scale;
    if (width > panel.width || height > panel.height)
        return std::nullopt;
    const auto& profile = profile_ ? *profile_ : scaled_alpha_gear;
    const auto source_scale = scale >= 2 && !profile.image_2x.empty() ? 2U : 1U;
    const auto image = source_scale == 2 ? profile.image_2x : profile.image_1x;
    const auto phase = elapsed / gear_interval + 1U;
    const auto frame = phase % profile.frames;
    // XNU-2050 vc_progress_task increments vc_progress_angle at each wrap.
    // The six samples describe a quarter turn, not a complete animation.
    const auto rotation =
        profile.rotate_on_wrap ? (phase / profile.frames) % 4U : 0U;
    const auto source_width = gear_width * source_scale;
    const auto left = (panel.width - width) / 2U;
    const auto top = (panel.height - height) / 2U;
    std::vector<std::uint32_t> pixels(panel.pixel_count(), 0xff000000U);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto source_x = x * source_scale / scale;
            const auto source_y = y * source_scale / scale;
            auto sample_x = source_x;
            auto sample_y = source_y;
            // The inverse sampling transform is vc_rotate_matr[kDataRotate].
            switch (rotation) {
            case 1:
                sample_x = source_y;
                sample_y = source_width - 1U - source_x;
                break;
            case 2:
                sample_x = source_width - 1U - source_x;
                sample_y = source_width - 1U - source_y;
                break;
            case 3:
                sample_x = source_width - 1U - source_y;
                sample_y = source_x;
                break;
            default:
                break;
            }
            const auto alpha = static_cast<std::uint32_t>(
                image[frame * source_width * source_width +
                      sample_y * source_width + sample_x]);
            // CONFIG_VC_PROGRESS_WHITE's alpha blend over the black console.
            const auto gray = 255U - alpha;
            pixels[(top + y) * panel.width + left + x] =
                0xff000000U | gray * 0x00010101U;
        }
    }
    return pixels;
}

} // namespace ilemu
