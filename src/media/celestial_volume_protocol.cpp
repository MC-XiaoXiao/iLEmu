#include "ilemu/celestial_volume_protocol.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace ilemu::celestial_volume_protocol {
namespace {

constexpr std::uint32_t category_volume_reply_identifier = 1138;
constexpr std::uint32_t source_create_request_identifier = 1027;
constexpr std::uint32_t source_create_reply_identifier = 1127;
constexpr std::uint32_t source_property_request_identifier = 1031;
constexpr std::size_t return_code_offset = 32;
constexpr std::size_t volume_offset = 36;
constexpr std::size_t category_present_offset = 40;
constexpr std::size_t category_length_offset = 44;
constexpr std::size_t category_offset = 48;
constexpr std::uint32_t maximum_category_length = 128;
constexpr std::size_t source_identifier_offset = 32;
constexpr std::size_t source_property_length_offset = 40;
constexpr std::size_t source_property_offset = 44;
constexpr std::uint32_t maximum_source_property_length = 128;
constexpr std::uint32_t maximum_source_path_length = 4096;

std::uint32_t read_word(std::span<const std::byte> bytes,
                        std::size_t offset) {
  std::uint32_t value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

bool printable_ascii(std::span<const std::byte> bytes) {
  return std::ranges::all_of(bytes, [](std::byte value) {
    const auto character = std::to_integer<unsigned char>(value);
    return character >= 0x20U && character <= 0x7eU;
  });
}

std::optional<SourceFloatProperty>
decode_float_property(std::uint32_t identifier,
                      std::span<const std::byte> bytes) {
  if (identifier != source_property_request_identifier ||
      bytes.size() < source_property_offset) {
    return std::nullopt;
  }
  const auto encoded_length = read_word(bytes, source_property_length_offset);
  if (encoded_length < 2U ||
      encoded_length > maximum_source_property_length ||
      encoded_length > bytes.size() - source_property_offset) {
    return std::nullopt;
  }
  const auto property_bytes = bytes.subspan(source_property_offset,
                                             encoded_length - 1U);
  if (!printable_ascii(property_bytes) ||
      bytes[source_property_offset + encoded_length - 1U] != std::byte{}) {
    return std::nullopt;
  }
  const auto value_size_offset =
      (source_property_offset + encoded_length + 3U) & ~std::size_t{3U};
  const auto value_offset = value_size_offset + sizeof(std::uint32_t);
  if (value_offset > bytes.size() ||
      sizeof(std::uint32_t) > bytes.size() - value_offset ||
      read_word(bytes, value_size_offset) != sizeof(float)) {
    return std::nullopt;
  }
  const auto value = std::bit_cast<float>(read_word(bytes, value_offset));
  if (!std::isfinite(value))
    return std::nullopt;
  return SourceFloatProperty{
      read_word(bytes, source_identifier_offset),
      std::string{reinterpret_cast<const char *>(property_bytes.data()),
                  property_bytes.size()},
      value};
}

} // namespace

std::optional<CategoryVolume>
decode_reply(std::uint32_t identifier, std::span<const std::byte> bytes) {
  if (identifier != category_volume_reply_identifier ||
      bytes.size() < category_offset ||
      read_word(bytes, return_code_offset) != 0 ||
      read_word(bytes, category_present_offset) == 0) {
    return std::nullopt;
  }

  const auto length = read_word(bytes, category_length_offset);
  if (length == 0 || length > maximum_category_length ||
      length > bytes.size() - category_offset) {
    return std::nullopt;
  }
  const auto value = std::bit_cast<float>(read_word(bytes, volume_offset));
  if (!std::isfinite(value) || value < 0.0F || value > 1.0F)
    return std::nullopt;

  std::string category;
  category.reserve(length);
  for (std::size_t index = 0; index < length; ++index) {
    const auto character =
        std::to_integer<unsigned char>(bytes[category_offset + index]);
    if (character < 0x20U || character > 0x7eU)
      return std::nullopt;
    category.push_back(static_cast<char>(character));
  }
  return CategoryVolume{std::move(category), value};
}

std::optional<std::string>
decode_source_create_path(std::uint32_t identifier,
                          std::span<const std::byte> payload) {
  if (identifier != source_create_request_identifier || payload.empty() ||
      payload.size() > maximum_source_path_length) {
    return std::nullopt;
  }
  const auto terminator = std::ranges::find(payload, std::byte{});
  const auto path_bytes = payload.first(static_cast<std::size_t>(
      std::distance(payload.begin(), terminator)));
  if (path_bytes.empty())
    return std::nullopt;
  if (!printable_ascii(path_bytes) || path_bytes.front() != std::byte{'/'})
    return std::nullopt;
  return std::string{reinterpret_cast<const char *>(path_bytes.data()),
                     path_bytes.size()};
}

std::optional<SourceCreateReply>
decode_source_create_reply(std::uint32_t identifier,
                           std::span<const std::byte> bytes) {
  if (identifier != source_create_reply_identifier ||
      bytes.size() < source_identifier_offset + sizeof(std::uint32_t) * 2U ||
      read_word(bytes, return_code_offset) != 0) {
    return std::nullopt;
  }
  const auto source = read_word(bytes, source_identifier_offset + 4U);
  return source == 0 ? std::nullopt
                     : std::optional{SourceCreateReply{source}};
}

std::optional<SourceFloatProperty>
decode_source_float_property_request(std::uint32_t identifier,
                                     std::span<const std::byte> bytes) {
  return decode_float_property(identifier, bytes);
}

} // namespace ilemu::celestial_volume_protocol
