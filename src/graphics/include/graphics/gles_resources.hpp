// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Manage GLES textures, framebuffers and their backing surface
// lifetimes.

#pragma once

#include "graphics/gles_pixel_unpack.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace ilemu {

class AddressSpace;
class GlesRenderer;
class GlesRendererOwner;
class GlesTextureTarget;
class HostSurface;
class SurfaceStore;

class GlesResourceStore {
public:
    struct TextureLevel {
        TextureLevel() = default;
        TextureLevel(std::uint32_t image_width, std::uint32_t image_height,
            std::uint32_t format, std::vector<std::uint32_t> pixels,
            std::optional<std::uint32_t> surface, std::uint64_t image_revision,
            std::shared_ptr<HostSurface> native_surface = { },
            std::uint64_t generation = 0)
            : width { image_width }, height { image_height },
              internal_format { format }, surface_id { surface },
              revision { image_revision }, host_surface { std::move(native_surface) },
              host_generation { generation }
        {
            set_pixels(std::move(pixels));
        }

        [[nodiscard]] std::span<const std::uint32_t> pixels() const
        {
            return argb_ ? std::span<const std::uint32_t> { *argb_ }
                         : std::span<const std::uint32_t> { };
        }
        void set_pixels(std::vector<std::uint32_t> pixels)
        {
            argb_ = pixels.empty() ? nullptr
                : std::make_shared<std::vector<std::uint32_t>>(std::move(pixels));
        }
        void assign_pixels(std::span<const std::uint32_t> pixels)
        {
            if (argb_ && argb_.use_count() == 1)
                argb_->assign(pixels.begin(), pixels.end());
            else
                argb_ = std::make_shared<std::vector<std::uint32_t>>(
                    pixels.begin(), pixels.end());
        }
        [[nodiscard]] std::vector<std::uint32_t>& writable_pixels()
        {
            if (!argb_)
                argb_ = std::make_shared<std::vector<std::uint32_t>>();
            else if (argb_.use_count() != 1)
                argb_ = std::make_shared<std::vector<std::uint32_t>>(*argb_);
            return *argb_;
        }

        std::uint32_t width { };
        std::uint32_t height { };
        std::uint32_t internal_format { };
        std::optional<std::uint32_t> surface_id;
        std::uint64_t revision { };
        std::shared_ptr<HostSurface> host_surface;
        std::uint64_t host_generation { };
        bool render_target_inverted_vertical { };
        // Generation represented by argb, independently of the native image.
        std::uint64_t argb_generation { };
        // A copied/forked texture retains its exact native incarnation until
        // its last logical or pending host user releases it.
        std::shared_ptr<GlesTextureTarget> native_lifetime { };

    private:
        // Queued draws retain immutable pixels. Only a later guest mutation
        // separates shared storage; snapshots never copy the image payload.
        std::shared_ptr<std::vector<std::uint32_t>> argb_;
    };
    struct Texture {
        std::uint32_t name { };
        std::map<std::uint32_t, TextureLevel> levels;
        std::map<std::uint32_t, std::uint32_t> parameters;
    };
    struct Buffer {
        std::uint32_t name { };
        std::uint32_t usage { };
        std::vector<std::byte> bytes;
    };

    void reset();
    void inherit_state(const GlesResourceStore& parent);

    [[nodiscard]] std::uint32_t generate_texture();
    [[nodiscard]] std::uint32_t generate_buffer();
    void ensure_texture(std::uint32_t name);
    void ensure_buffer(std::uint32_t name);
    void erase_texture(std::uint32_t name);
    void erase_buffer(std::uint32_t name);
    [[nodiscard]] bool has_texture(std::uint32_t name) const;
    [[nodiscard]] bool has_buffer(std::uint32_t name) const;
    enum class TextureSnapshot { CpuPixels, NativeImages };
    [[nodiscard]] std::size_t texture_snapshot_bytes(
        std::span<const std::uint32_t> names, std::size_t limit,
        TextureSnapshot content = TextureSnapshot::CpuPixels) const;
    // NativeImages requires the caller to retain each physical owner and
    // order surface reads. GPU-only images do not copy stale CPU caches.
    [[nodiscard]] GlesResourceStore snapshot_textures(
        std::span<const std::uint32_t> names,
        TextureSnapshot content = TextureSnapshot::CpuPixels) const;

    [[nodiscard]] std::uint32_t upload_texture_2d(AddressSpace& memory,
        std::uint32_t name, std::uint32_t level, std::uint32_t internal_format,
        std::uint32_t width, std::uint32_t height, std::uint32_t format,
        std::uint32_t type, std::uint32_t pixels, const GlesPixelUnpack& unpack,
        GlesClientBitmapProfile client_bitmap_profile);
    [[nodiscard]] std::uint32_t allocate_texture_2d(std::uint32_t name,
        std::uint32_t level, std::uint32_t internal_format, std::uint32_t width,
        std::uint32_t height);
    [[nodiscard]] std::uint32_t upload_compressed_texture_2d(
        AddressSpace& memory, std::uint32_t name, std::uint32_t level,
        std::uint32_t internal_format, std::uint32_t width,
        std::uint32_t height, std::uint32_t image_size, std::uint32_t pixels);
    [[nodiscard]] std::uint32_t update_texture_2d(AddressSpace& memory,
        std::uint32_t name, std::uint32_t level, std::uint32_t x,
        std::uint32_t y, std::uint32_t width, std::uint32_t height,
        std::uint32_t format, std::uint32_t type, std::uint32_t pixels,
        const GlesPixelUnpack& unpack,
        GlesClientBitmapProfile client_bitmap_profile);
    [[nodiscard]] std::uint32_t set_texture_parameter(
        std::uint32_t name, std::uint32_t parameter, std::uint32_t value);
    [[nodiscard]] std::uint32_t import_surface_texture(AddressSpace& memory,
        std::uint32_t name, const SurfaceStore& surfaces,
        std::uint32_t surface_id, bool render_target_inverted_vertical);
    [[nodiscard]] std::uint32_t refresh_surface_texture(
        AddressSpace& memory, std::uint32_t name, const SurfaceStore& surfaces);
    [[nodiscard]] bool needs_texture_render_target(std::uint32_t name,
        std::uint32_t level, bool writable) const;
    [[nodiscard]] std::shared_ptr<HostSurface> ensure_texture_render_target(
        std::uint32_t name, std::uint32_t level,
        std::shared_ptr<GlesRenderer> renderer,
        std::shared_ptr<GlesRendererOwner> lifetime, std::uint64_t owner,
        std::uint64_t target_namespace, bool writable);
    [[nodiscard]] bool commit_texture_render_target(std::uint32_t name,
        std::uint32_t level, std::span<const std::uint32_t> pixels);
    void update_texture_render_target_generation(
        std::uint32_t name, std::uint32_t level);
    [[nodiscard]] std::uint32_t upload_buffer(AddressSpace& memory,
        std::uint32_t name, std::uint32_t size, std::uint32_t data,
        std::uint32_t usage);
    [[nodiscard]] std::uint32_t update_buffer(AddressSpace& memory,
        std::uint32_t name, std::uint32_t offset, std::uint32_t size,
        std::uint32_t data);

    [[nodiscard]] Texture* texture(std::uint32_t name);
    [[nodiscard]] const Texture* texture(std::uint32_t name) const;
    [[nodiscard]] const Buffer* buffer(std::uint32_t name) const;

private:
    friend class GlesPreparedTextures;
    [[nodiscard]] std::uint64_t allocate_texture_revision();

    std::map<std::uint32_t, Texture> textures_;
    std::map<std::uint32_t, Buffer> buffers_;
    std::set<std::uint32_t> generated_textures_;
    std::set<std::uint32_t> generated_buffers_;
    std::uint32_t next_texture_ { 1 };
    std::uint32_t next_buffer_ { 1 };
    std::uint64_t next_texture_revision_ { 1 };
};

} // namespace ilemu
