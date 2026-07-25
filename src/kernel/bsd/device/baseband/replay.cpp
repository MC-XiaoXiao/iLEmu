#include "ilemu/baseband_replay.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace ilemu::bsd::baseband_device {

std::vector<std::byte> load_replay_file(const std::filesystem::path &path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    throw std::runtime_error{"cannot open baseband replay input: " +
                             path.string()};
  }
  const std::vector<char> source{std::istreambuf_iterator<char>{stream},
                                 std::istreambuf_iterator<char>{}};
  if (stream.bad()) {
    throw std::runtime_error{"cannot read baseband replay input: " +
                             path.string()};
  }
  std::vector<std::byte> bytes;
  bytes.reserve(source.size());
  for (const auto byte : source) {
    bytes.push_back(static_cast<std::byte>(
        static_cast<unsigned char>(byte)));
  }
  return bytes;
}

void write_capture_file(const std::filesystem::path &path,
                        std::span<const std::byte> bytes) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  if (!stream) {
    throw std::runtime_error{"cannot open baseband capture output: " +
                             path.string()};
  }
  stream.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!stream) {
    throw std::runtime_error{"cannot write baseband capture output: " +
                             path.string()};
  }
}

} // namespace ilemu::bsd::baseband_device
