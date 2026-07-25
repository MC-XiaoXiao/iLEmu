#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace ilemu {

class AddressSpace;
class SurfaceStore;

class GlesResourceStore {
public:
    struct TextureLevel {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t internal_format{};
        std::vector<std::uint32_t> argb;
        std::optional<std::uint32_t> surface_id;
        std::uint64_t revision{};
    };
    struct Texture {
        std::uint32_t name{};
        std::map<std::uint32_t, TextureLevel> levels;
        std::map<std::uint32_t, std::uint32_t> parameters;
    };
    struct Buffer {
        std::uint32_t name{};
        std::uint32_t usage{};
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

    [[nodiscard]] std::uint32_t upload_texture_2d(
        AddressSpace& memory, std::uint32_t name, std::uint32_t level,
        std::uint32_t internal_format, std::uint32_t width,
        std::uint32_t height, std::uint32_t format, std::uint32_t type,
        std::uint32_t pixels, std::uint32_t alignment);
    [[nodiscard]] std::uint32_t update_texture_2d(
        AddressSpace& memory, std::uint32_t name, std::uint32_t level,
        std::uint32_t x, std::uint32_t y, std::uint32_t width,
        std::uint32_t height, std::uint32_t format, std::uint32_t type,
        std::uint32_t pixels, std::uint32_t alignment);
    [[nodiscard]] std::uint32_t set_texture_parameter(
        std::uint32_t name, std::uint32_t parameter,
        std::uint32_t value);
    [[nodiscard]] std::uint32_t import_surface_texture(
        AddressSpace& memory, std::uint32_t name,
        const SurfaceStore& surfaces, std::uint32_t surface_id);
    [[nodiscard]] std::uint32_t refresh_surface_texture(
        AddressSpace& memory, std::uint32_t name,
        const SurfaceStore& surfaces);

    [[nodiscard]] std::uint32_t upload_buffer(
        AddressSpace& memory, std::uint32_t name, std::uint32_t size,
        std::uint32_t data, std::uint32_t usage);
    [[nodiscard]] std::uint32_t update_buffer(
        AddressSpace& memory, std::uint32_t name, std::uint32_t offset,
        std::uint32_t size, std::uint32_t data);

    [[nodiscard]] const Texture* texture(std::uint32_t name) const;
    [[nodiscard]] const Buffer* buffer(std::uint32_t name) const;

private:
    [[nodiscard]] std::uint64_t allocate_texture_revision();

    std::map<std::uint32_t, Texture> textures_;
    std::map<std::uint32_t, Buffer> buffers_;
    std::set<std::uint32_t> generated_textures_;
    std::set<std::uint32_t> generated_buffers_;
    std::uint32_t next_texture_{1};
    std::uint32_t next_buffer_{1};
    std::uint64_t next_texture_revision_{1};
};

}  // namespace ilemu
