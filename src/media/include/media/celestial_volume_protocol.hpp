#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ilemu::celestial_volume_protocol {

struct CategoryVolume {
    std::string category;
    float value { };
};

struct SourceFloatProperty {
    std::uint32_t source { };
    std::string property;
    float value { };
};

struct SourceCreateReply {
    std::uint32_t source { };
};

// Decode the mediaserverd response that reports the canonical category
// affected by an AVSystemController volume operation. Callers remain agnostic
// to SpringBoard and Preferences; both clients use the same service protocol.
[[nodiscard]] std::optional<CategoryVolume> decode_reply(
    std::uint32_t identifier, std::span<const std::byte> bytes);

// FigMovie's create request transports the canonical filesystem path out of
// line. The response returns a server-side movie identifier. The Mach layer
// correlates the two through the request's reply-port object.
[[nodiscard]] std::optional<std::string> decode_source_create_path(
    std::uint32_t identifier, std::span<const std::byte> payload);
[[nodiscard]] std::optional<SourceCreateReply> decode_source_create_reply(
    std::uint32_t identifier, std::span<const std::byte> bytes);

// Fig media clients set typed float properties (including playback rate and
// user volume) on one server-side movie object. Preserve that object identity
// so overlapping previews cannot affect one another.
[[nodiscard]] std::optional<SourceFloatProperty>
decode_source_float_property_request(
    std::uint32_t identifier, std::span<const std::byte> bytes);

} // namespace ilemu::celestial_volume_protocol
