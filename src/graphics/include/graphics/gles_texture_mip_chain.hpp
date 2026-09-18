// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "graphics/gles_resources.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace ilemu {

// A view of the guest's complete CPU-backed mip pyramid. Surface-backed and
// incomplete textures keep the existing base-level sampling path.
class GlesTextureMipChain {
public:
    explicit GlesTextureMipChain(
        const GlesResourceStore::Texture& texture, bool rectangle = false)
    {
        const auto base = texture.levels.find(0);
        if (base == texture.levels.end())
            return;
        levels_[count_++] = &base->second;
        if (rectangle || base->second.host_surface || base->second.width == 0 ||
            base->second.height == 0)
            return;
        auto width = base->second.width;
        auto height = base->second.height;
        while (width > 1 || height > 1) {
            width = std::max(1U, width / 2);
            height = std::max(1U, height / 2);
            const auto found = texture.levels.find(count_);
            if (found == texture.levels.end() || found->second.width != width ||
                found->second.height != height || found->second.host_surface ||
                found->second.internal_format != base->second.internal_format ||
                found->second.argb.size() !=
                    static_cast<std::size_t>(width) * height) {
                count_ = 1;
                return;
            }
            levels_[count_++] = &found->second;
        }
    }

    [[nodiscard]] std::span<const GlesResourceStore::TextureLevel* const>
    levels() const
    {
        return { levels_.data(), count_ };
    }

private:
    std::array<const GlesResourceStore::TextureLevel*, 32> levels_ { };
    std::uint32_t count_ { };
};

} // namespace ilemu
