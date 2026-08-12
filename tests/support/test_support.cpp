#include "test_support.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace ilemu::test {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

std::filesystem::path find_existing(
    std::span<const std::filesystem::path> candidates) {
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

std::string read_binary_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(input.good(), "failed to open test fixture: " + path.string());
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

int run_suite(std::string_view name, void (*suite)()) {
    try {
        suite();
        std::cout << name << " tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << name << " test failure: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace ilemu::test
