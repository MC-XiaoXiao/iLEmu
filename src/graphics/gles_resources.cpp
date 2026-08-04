#include "ilemu/gles_resources.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "ilemu/address_space.hpp"
#include "ilemu/gles_abi.hpp"
#include "ilemu/host_graphics.hpp"
#include "ilemu/surface_store.hpp"

namespace ilemu {
namespace {

struct PixelLayout {
    std::uint32_t bytes_per_pixel{};
};

std::optional<PixelLayout> pixel_layout(
    std::uint32_t format, std::uint32_t type) {
    using namespace gles_abi;
    if (type == unsigned_byte) {
        switch (format) {
        case alpha:
        case luminance: return PixelLayout{1};
        case luminance_alpha: return PixelLayout{2};
        case rgb: return PixelLayout{3};
        case rgba:
        case bgra_apple: return PixelLayout{4};
        default: return std::nullopt;
        }
    }
    if ((type == unsigned_short_5_6_5 && format == rgb) ||
        ((type == unsigned_short_4_4_4_4 ||
          type == unsigned_short_5_5_5_1) && format == rgba)) {
        return PixelLayout{2};
    }
    return std::nullopt;
}

std::uint32_t expand(std::uint32_t value, std::uint32_t maximum) {
    return (value * 255U + maximum / 2U) / maximum;
}

std::uint32_t decode_pixel(
    std::span<const std::byte> bytes, std::uint32_t format,
    std::uint32_t type) {
    using namespace gles_abi;
    const auto byte = [&](std::size_t index) {
        return std::to_integer<std::uint32_t>(bytes[index]);
    };
    std::uint32_t red{};
    std::uint32_t green{};
    std::uint32_t blue{};
    std::uint32_t alpha_value{255};
    if (type == unsigned_byte) {
        if (format == rgba) {
            red = byte(0); green = byte(1); blue = byte(2); alpha_value = byte(3);
        } else if (format == bgra_apple) {
            blue = byte(0); green = byte(1); red = byte(2); alpha_value = byte(3);
        } else if (format == rgb) {
            red = byte(0); green = byte(1); blue = byte(2);
        } else if (format == luminance) {
            red = green = blue = byte(0);
        } else if (format == luminance_alpha) {
            red = green = blue = byte(0); alpha_value = byte(1);
        } else if (format == alpha) {
            red = green = blue = 255U;
            alpha_value = byte(0);
        }
    } else {
        const auto packed = byte(0) | (byte(1) << 8U);
        if (type == unsigned_short_5_6_5) {
            red = expand((packed >> 11U) & 0x1fU, 0x1fU);
            green = expand((packed >> 5U) & 0x3fU, 0x3fU);
            blue = expand(packed & 0x1fU, 0x1fU);
        } else if (type == unsigned_short_4_4_4_4) {
            red = expand((packed >> 12U) & 0x0fU, 0x0fU);
            green = expand((packed >> 8U) & 0x0fU, 0x0fU);
            blue = expand((packed >> 4U) & 0x0fU, 0x0fU);
            alpha_value = expand(packed & 0x0fU, 0x0fU);
        } else {
            red = expand((packed >> 11U) & 0x1fU, 0x1fU);
            green = expand((packed >> 6U) & 0x1fU, 0x1fU);
            blue = expand((packed >> 1U) & 0x1fU, 0x1fU);
            alpha_value = (packed & 1U) != 0 ? 255U : 0U;
        }
    }
    return (alpha_value << 24U) | (red << 16U) | (green << 8U) | blue;
}

bool valid_alignment(std::uint32_t alignment) {
    return alignment == 1 || alignment == 2 || alignment == 4 ||
           alignment == 8;
}

std::optional<std::vector<std::uint32_t>> decode_image(
    AddressSpace& memory, std::uint32_t width, std::uint32_t height,
    std::uint32_t format, std::uint32_t type, std::uint32_t pixels,
    std::uint32_t alignment) {
    const auto layout = pixel_layout(format, type);
    if (!layout || !valid_alignment(alignment)) return std::nullopt;
    const auto row_bytes = static_cast<std::uint64_t>(width) *
                           layout->bytes_per_pixel;
    const auto stride = (row_bytes + alignment - 1U) &
                        ~static_cast<std::uint64_t>(alignment - 1U);
    const auto total = height == 0 ? 0 :
        stride * (height - 1U) + row_bytes;
    if (total > gles_abi::maximum_resource_bytes ||
        total > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    std::vector<std::uint32_t> result(
        static_cast<std::size_t>(width) * height, 0);
    if (pixels == 0 || total == 0) return result;
    const auto source = memory.read_bytes(pixels, static_cast<std::size_t>(total));
    if (!source) return std::nullopt;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>(
                static_cast<std::uint64_t>(y) * stride +
                static_cast<std::uint64_t>(x) * layout->bytes_per_pixel);
            result[static_cast<std::size_t>(y) * width + x] = decode_pixel(
                std::span<const std::byte>{*source}.subspan(
                    offset, layout->bytes_per_pixel), format, type);
        }
    }
    return result;
}

std::optional<GlesResourceStore::TextureLevel> decode_surface(
    AddressSpace& memory, const SurfaceStore& surfaces,
    const SurfaceStore::Backing& backing) {
    if (backing.width > gles_abi::maximum_texture_dimension ||
        backing.height > gles_abi::maximum_texture_dimension) {
        return std::nullopt;
    }
    const auto pixel_count = static_cast<std::uint64_t>(backing.width) *
                             backing.height;
    if (pixel_count > gles_abi::maximum_resource_bytes /
                          sizeof(std::uint32_t)) {
        return std::nullopt;
    }
    auto pixels = surfaces.read_argb(memory, backing.id);
    if (!pixels || pixels->size() != pixel_count) return std::nullopt;
    return GlesResourceStore::TextureLevel{
        backing.width, backing.height, gles_abi::bgra_apple,
        std::move(*pixels), backing.id, 0, {}, 0};
}

}  // namespace

std::uint64_t GlesResourceStore::allocate_texture_revision() {
    const auto revision = next_texture_revision_++;
    if (next_texture_revision_ == 0)
        next_texture_revision_ = 1;
    return revision;
}

void GlesResourceStore::reset() {
    textures_.clear();
    buffers_.clear();
    generated_textures_.clear();
    generated_buffers_.clear();
    next_texture_ = 1;
    next_buffer_ = 1;
    next_texture_revision_ = 1;
}

void GlesResourceStore::inherit_state(const GlesResourceStore& parent) {
    textures_ = parent.textures_;
    buffers_ = parent.buffers_;
    generated_textures_ = parent.generated_textures_;
    generated_buffers_ = parent.generated_buffers_;
    next_texture_ = parent.next_texture_;
    next_buffer_ = parent.next_buffer_;
    next_texture_revision_ = parent.next_texture_revision_;
}

std::uint32_t GlesResourceStore::generate_texture() {
    const auto name = next_texture_++;
    generated_textures_.insert(name);
    return name;
}

std::uint32_t GlesResourceStore::generate_buffer() {
    const auto name = next_buffer_++;
    generated_buffers_.insert(name);
    return name;
}

void GlesResourceStore::ensure_texture(std::uint32_t name) {
    if (name == 0) return;
    generated_textures_.insert(name);
    textures_.try_emplace(name, Texture{name, {}, {}});
    next_texture_ = std::max(next_texture_, name + 1U);
}

void GlesResourceStore::ensure_buffer(std::uint32_t name) {
    if (name == 0) return;
    generated_buffers_.insert(name);
    buffers_.try_emplace(name, Buffer{name, 0, {}});
    next_buffer_ = std::max(next_buffer_, name + 1U);
}

void GlesResourceStore::erase_texture(std::uint32_t name) {
    textures_.erase(name);
    generated_textures_.erase(name);
}

void GlesResourceStore::erase_buffer(std::uint32_t name) {
    buffers_.erase(name);
    generated_buffers_.erase(name);
}

bool GlesResourceStore::has_texture(std::uint32_t name) const {
    return textures_.contains(name);
}

bool GlesResourceStore::has_buffer(std::uint32_t name) const {
    return buffers_.contains(name);
}

std::uint32_t GlesResourceStore::upload_texture_2d(
    AddressSpace& memory, std::uint32_t name, std::uint32_t level,
    std::uint32_t internal_format, std::uint32_t width,
    std::uint32_t height, std::uint32_t format, std::uint32_t type,
    std::uint32_t pixels, std::uint32_t alignment) {
    if (name == 0 || !textures_.contains(name)) {
        return gles_abi::invalid_operation;
    }
    if (width > gles_abi::maximum_texture_dimension ||
        height > gles_abi::maximum_texture_dimension ||
        static_cast<std::uint64_t>(width) * height * sizeof(std::uint32_t) >
            gles_abi::maximum_resource_bytes) {
        return gles_abi::invalid_value;
    }
    if (!pixel_layout(format, type)) return gles_abi::invalid_enum;
    auto decoded = decode_image(
        memory, width, height, format, type, pixels, alignment);
    if (!decoded) return gles_abi::invalid_value;
    textures_.at(name).levels.insert_or_assign(
        level, TextureLevel{
                   width, height, internal_format, std::move(*decoded),
                   std::nullopt, allocate_texture_revision(), {}, 0});
    return gles_abi::no_error;
}

std::uint32_t GlesResourceStore::update_texture_2d(
    AddressSpace& memory, std::uint32_t name, std::uint32_t level,
    std::uint32_t x, std::uint32_t y, std::uint32_t width,
    std::uint32_t height, std::uint32_t format, std::uint32_t type,
    std::uint32_t pixels, std::uint32_t alignment) {
    auto texture = textures_.find(name);
    if (name == 0 || texture == textures_.end()) {
        return gles_abi::invalid_operation;
    }
    auto destination = texture->second.levels.find(level);
    if (destination == texture->second.levels.end()) {
        return gles_abi::invalid_operation;
    }
    if (x > destination->second.width || y > destination->second.height ||
        width > destination->second.width - x ||
        height > destination->second.height - y || pixels == 0) {
        return gles_abi::invalid_value;
    }
    if (!pixel_layout(format, type)) return gles_abi::invalid_enum;
    const auto decoded = decode_image(
        memory, width, height, format, type, pixels, alignment);
    if (!decoded) return gles_abi::invalid_value;
    for (std::uint32_t row = 0; row < height; ++row) {
        std::copy_n(
            decoded->begin() + static_cast<std::size_t>(row) * width, width,
            destination->second.argb.begin() +
                static_cast<std::size_t>(y + row) * destination->second.width + x);
    }
    destination->second.revision = allocate_texture_revision();
    return gles_abi::no_error;
}

std::uint32_t GlesResourceStore::set_texture_parameter(
    std::uint32_t name, std::uint32_t parameter, std::uint32_t value) {
    auto texture = textures_.find(name);
    if (name == 0 || texture == textures_.end()) {
        return gles_abi::invalid_operation;
    }
    texture->second.parameters.insert_or_assign(parameter, value);
    return gles_abi::no_error;
}

std::uint32_t GlesResourceStore::import_surface_texture(
    AddressSpace& memory, std::uint32_t name, const SurfaceStore& surfaces,
    std::uint32_t surface_id, bool render_target_inverted_vertical) {
    auto texture = textures_.find(name);
    if (name == 0 || texture == textures_.end()) {
        return gles_abi::invalid_operation;
    }
    const auto backing = surfaces.find(surface_id);
    if (!backing) return gles_abi::invalid_value;
    if (backing->pixel_format != surface_pixel_format_bgra &&
        !surface_is_yuv422(backing->pixel_format)) {
        return gles_abi::invalid_enum;
    }
    const auto host_surface = surfaces.host_surface(surface_id);
    if (host_surface &&
        host_surface->gpu_generation() > host_surface->cpu_generation()) {
        TextureLevel imported;
        imported.width = backing->width;
        imported.height = backing->height;
        imported.internal_format = gles_abi::bgra_apple;
        imported.surface_id = backing->id;
        imported.revision = allocate_texture_revision();
        imported.host_generation = host_surface->gpu_generation();
        imported.host_surface = host_surface;
        imported.render_target_inverted_vertical =
            render_target_inverted_vertical;
        texture->second.levels.insert_or_assign(0, std::move(imported));
        return gles_abi::no_error;
    }
    auto decoded = decode_surface(memory, surfaces, *backing);
    if (!decoded) return gles_abi::invalid_value;
    decoded->host_surface = host_surface;
    decoded->host_generation =
        host_surface ? host_surface->cpu_generation() : 0;
    decoded->revision = allocate_texture_revision();
    decoded->render_target_inverted_vertical =
        render_target_inverted_vertical;
    texture->second.levels.insert_or_assign(0, std::move(*decoded));
    return gles_abi::no_error;
}

std::uint32_t GlesResourceStore::refresh_surface_texture(
    AddressSpace& memory, std::uint32_t name, const SurfaceStore& surfaces) {
    auto texture = textures_.find(name);
    if (name == 0 || texture == textures_.end()) {
        return gles_abi::no_error;
    }
    auto level = texture->second.levels.find(0);
    if (level == texture->second.levels.end() ||
        !level->second.surface_id) {
        return gles_abi::no_error;
    }
    const auto backing = surfaces.find(*level->second.surface_id);
    if (!backing) return gles_abi::invalid_operation;
    const auto render_target_inverted_vertical =
        level->second.render_target_inverted_vertical;
    const auto host_surface = surfaces.host_surface(*level->second.surface_id);
    if (host_surface &&
        host_surface->gpu_generation() > host_surface->cpu_generation()) {
        const auto generation = host_surface->gpu_generation();
        if (level->second.host_surface == host_surface &&
            level->second.host_generation == generation &&
            level->second.width == backing->width &&
            level->second.height == backing->height) {
            return gles_abi::no_error;
        }
        TextureLevel refreshed;
        refreshed.width = backing->width;
        refreshed.height = backing->height;
        refreshed.internal_format = gles_abi::bgra_apple;
        refreshed.surface_id = backing->id;
        refreshed.revision = allocate_texture_revision();
        refreshed.host_surface = host_surface;
        refreshed.host_generation = generation;
        refreshed.render_target_inverted_vertical =
            render_target_inverted_vertical;
        level->second = std::move(refreshed);
        return gles_abi::no_error;
    }
    auto decoded = decode_surface(memory, surfaces, *backing);
    if (!decoded) {
        return (backing->pixel_format == surface_pixel_format_bgra ||
                surface_is_yuv422(backing->pixel_format))
                   ? gles_abi::invalid_value
                   : gles_abi::invalid_enum;
    }
    decoded->host_surface = host_surface;
    decoded->host_generation =
        host_surface ? host_surface->cpu_generation() : 0;
    decoded->render_target_inverted_vertical =
        render_target_inverted_vertical;
    if (decoded->width == level->second.width &&
        decoded->height == level->second.height &&
        decoded->internal_format == level->second.internal_format &&
        decoded->surface_id == level->second.surface_id &&
        decoded->host_surface == level->second.host_surface &&
        decoded->host_generation == level->second.host_generation &&
        decoded->render_target_inverted_vertical ==
            level->second.render_target_inverted_vertical &&
        decoded->argb == level->second.argb) {
        return gles_abi::no_error;
    }
    decoded->revision = allocate_texture_revision();
    level->second = std::move(*decoded);
    return gles_abi::no_error;
}

std::shared_ptr<HostSurface> GlesResourceStore::ensure_texture_render_target(
    std::uint32_t name, std::uint32_t level_index,
    HostGraphicsDevice& graphics, std::uint64_t owner,
    std::uint64_t surface) {
    auto texture = textures_.find(name);
    if (name == 0U || texture == textures_.end())
        return nullptr;
    auto level = texture->second.levels.find(level_index);
    if (level == texture->second.levels.end() || level->second.width == 0U ||
        level->second.height == 0U || level->second.surface_id ||
        level->second.argb.size() !=
            static_cast<std::size_t>(level->second.width) *
                level->second.height) {
        return nullptr;
    }
    const auto key = HostSurfaceKey{owner, surface};
    const auto descriptor = HostSurfaceDescriptor{
        level->second.width, level->second.height,
        level->second.width * 4U,
        surface_pixel_format_bgra, PerfSurfaceKind::GlesRenderTarget};
    if (!level->second.host_surface ||
        level->second.host_surface->key() != key ||
        level->second.host_surface->descriptor().width != descriptor.width ||
        level->second.host_surface->descriptor().height != descriptor.height) {
        level->second.host_surface =
            graphics.create_surface(key, descriptor, level->second.argb);
        if (!level->second.host_surface)
            return nullptr;
        level->second.host_generation =
            level->second.host_surface->cpu_generation();
    }
    // A texture's first row is its GL lower edge. Native host targets use a
    // top-left row origin, so ordinary framebuffer rendering reverses Y.
    level->second.render_target_inverted_vertical = true;
    return level->second.host_surface;
}

bool GlesResourceStore::commit_texture_render_target(
    std::uint32_t name, std::uint32_t level_index,
    std::span<const std::uint32_t> pixels) {
    auto texture = textures_.find(name);
    if (name == 0U || texture == textures_.end())
        return false;
    auto level = texture->second.levels.find(level_index);
    if (level == texture->second.levels.end() || level->second.surface_id ||
        !level->second.host_surface ||
        pixels.size() != static_cast<std::size_t>(level->second.width) *
                             level->second.height) {
        return false;
    }
    level->second.argb.assign(pixels.begin(), pixels.end());
    level->second.host_surface->replace_cpu(level->second.argb);
    level->second.host_generation =
        level->second.host_surface->cpu_generation();
    level->second.revision = allocate_texture_revision();
    return true;
}

void GlesResourceStore::update_texture_render_target_generation(
    std::uint32_t name, std::uint32_t level_index) {
    auto texture = textures_.find(name);
    if (name == 0U || texture == textures_.end())
        return;
    auto level = texture->second.levels.find(level_index);
    if (level == texture->second.levels.end() ||
        !level->second.host_surface) {
        return;
    }
    level->second.host_generation = std::max(
        level->second.host_surface->cpu_generation(),
        level->second.host_surface->gpu_generation());
}

bool GlesResourceStore::materialize_surface_textures(
    HostGraphicsDevice& graphics) {
    for (auto& [name, texture] : textures_) {
        static_cast<void>(name);
        for (auto& [index, level] : texture.levels) {
            static_cast<void>(index);
            if (!level.host_surface ||
                level.argb.size() ==
                    static_cast<std::size_t>(level.width) * level.height) {
                continue;
            }
            if (level.host_surface->gpu_generation() >
                    level.host_surface->cpu_generation() &&
                graphics.native_image(*level.host_surface).api ==
                    HostNativeImage::Api::None) {
                return false;
            }
            if (!graphics.map_cpu(*level.host_surface, true,
                                  PerfCpuMapReason::SoftwareFallback)) {
                return false;
            }
            auto mapping = level.host_surface->map_cpu(
                false, PerfCpuMapReason::SoftwareFallback);
            const auto& frame = mapping.frame();
            if (frame.width != level.width || frame.height != level.height ||
                frame.pixels.size() !=
                    static_cast<std::size_t>(level.width) * level.height) {
                return false;
            }
            level.argb = frame.pixels;
            level.host_generation = std::max(
                level.host_surface->cpu_generation(),
                level.host_surface->gpu_generation());
            level.revision = allocate_texture_revision();
        }
    }
    return true;
}

std::uint32_t GlesResourceStore::upload_buffer(
    AddressSpace& memory, std::uint32_t name, std::uint32_t size,
    std::uint32_t data, std::uint32_t usage) {
    auto buffer = buffers_.find(name);
    if (name == 0 || buffer == buffers_.end()) {
        return gles_abi::invalid_operation;
    }
    if (size > gles_abi::maximum_resource_bytes) {
        return gles_abi::out_of_memory;
    }
    std::vector<std::byte> bytes(size);
    if (data != 0 && size != 0) {
        const auto source = memory.read_bytes(data, size);
        if (!source) return gles_abi::invalid_value;
        bytes = *source;
    }
    buffer->second.usage = usage;
    buffer->second.bytes = std::move(bytes);
    return gles_abi::no_error;
}

std::uint32_t GlesResourceStore::update_buffer(
    AddressSpace& memory, std::uint32_t name, std::uint32_t offset,
    std::uint32_t size, std::uint32_t data) {
    auto buffer = buffers_.find(name);
    if (name == 0 || buffer == buffers_.end()) {
        return gles_abi::invalid_operation;
    }
    if (offset > buffer->second.bytes.size() ||
        size > buffer->second.bytes.size() - offset || data == 0) {
        return gles_abi::invalid_value;
    }
    const auto source = memory.read_bytes(data, size);
    if (!source) return gles_abi::invalid_value;
    std::copy(source->begin(), source->end(),
              buffer->second.bytes.begin() + offset);
    return gles_abi::no_error;
}

const GlesResourceStore::Texture* GlesResourceStore::texture(
    std::uint32_t name) const {
    const auto found = textures_.find(name);
    return found == textures_.end() ? nullptr : &found->second;
}

const GlesResourceStore::Buffer* GlesResourceStore::buffer(
    std::uint32_t name) const {
    const auto found = buffers_.find(name);
    return found == buffers_.end() ? nullptr : &found->second;
}

}  // namespace ilemu
