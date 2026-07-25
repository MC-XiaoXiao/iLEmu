#include "ilemu/darwin_sysctl.hpp"

#include <algorithm>
#include <iterator>

namespace ilemu::darwin::sysctl {
namespace {

void append_string(std::vector<std::byte> &result, std::string_view value) {
  std::transform(value.begin(), value.end(), std::back_inserter(result),
                 [](char character) {
                   return static_cast<std::byte>(character);
                 });
  result.push_back(std::byte{});
}

void align_word(std::vector<std::byte> &result) {
  constexpr std::size_t word_size = sizeof(std::uint32_t);
  while (result.size() % word_size != 0) result.push_back(std::byte{});
}

} // namespace

std::vector<std::byte>
encode_process_arguments(std::string_view executable_path,
                         std::span<const std::string> arguments,
                         std::span<const std::string> environment) {
  std::vector<std::byte> result;
  const auto argument_bytes = [](std::span<const std::string> values) {
    std::size_t size = 1;
    for (const auto &value : values) size += value.size() + 1U;
    return size;
  };
  result.reserve(executable_path.size() + 1U + sizeof(std::uint32_t) +
                 argument_bytes(arguments) + argument_bytes(environment));
  append_string(result, executable_path);
  align_word(result);
  for (const auto &argument : arguments) append_string(result, argument);
  result.push_back(std::byte{});
  for (const auto &variable : environment) append_string(result, variable);
  result.push_back(std::byte{});
  return result;
}

} // namespace ilemu::darwin::sysctl
