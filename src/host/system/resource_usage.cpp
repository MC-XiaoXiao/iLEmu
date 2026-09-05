#include "host/resource_usage.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace ilemu {
namespace {

    [[nodiscard]] std::optional<std::uint64_t> read_decimal_file(
        const std::filesystem::path& path)
    {
        std::ifstream input { path };
        std::string value;
        input >> value;
        if (!input || value.empty() || value == "max")
            return std::nullopt;
        std::size_t consumed { };
        try {
            const auto parsed = std::stoull(value, &consumed, 10);
            if (consumed != value.size())
                return std::nullopt;
            return static_cast<std::uint64_t>(parsed);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

} // namespace

[[nodiscard]] HostMemorySnapshot host_memory_snapshot()
{
    HostMemorySnapshot snapshot;
#if defined(__linux__)
    std::ifstream status { "/proc/self/status" };
    std::string line;
    while (std::getline(status, line)) {
        std::istringstream fields { line };
        std::string label;
        std::uint64_t value { };
        std::string unit;
        fields >> label >> value >> unit;
        if (!fields || unit != "kB")
            continue;
        if (value > std::numeric_limits<std::uint64_t>::max() / 1024U)
            continue;
        const auto bytes = value * 1024U;
        if (label == "VmRSS:") {
            snapshot.rss_bytes = bytes;
            snapshot.rss_known = true;
        }
        if (label == "VmHWM:") {
            snapshot.peak_rss_bytes = bytes;
            snapshot.peak_rss_known = true;
        }
        if (label == "VmSize:") {
            snapshot.virtual_bytes = bytes;
            snapshot.virtual_known = true;
        }
        if (label == "RssFile:") {
            snapshot.file_mapped_bytes = bytes;
            snapshot.file_mapped_known = true;
        }
    }
#endif
    return snapshot;
}

[[nodiscard]] HostMemoryBudgetSnapshot host_memory_budget_snapshot()
{
    HostMemoryBudgetSnapshot snapshot;
    const auto process_memory = host_memory_snapshot();
    snapshot.rss_bytes = process_memory.rss_bytes;
    snapshot.rss_known = process_memory.rss_known;
#if defined(__linux__)
    {
        std::ifstream meminfo { "/proc/meminfo" };
        std::string line;
        while (std::getline(meminfo, line)) {
            std::istringstream fields { line };
            std::string label;
            std::uint64_t value { };
            std::string unit;
            fields >> label >> value >> unit;
            if (!fields || unit != "kB" ||
                value > std::numeric_limits<std::uint64_t>::max() / 1024U) {
                continue;
            }
            const auto bytes = value * 1024U;
            if (label == "MemTotal:") {
                snapshot.physical_bytes = bytes;
                snapshot.physical_known = true;
            }
            if (label == "MemAvailable:") {
                snapshot.available_bytes = bytes;
                snapshot.available_known = true;
            }
        }
    }

    std::filesystem::path cgroup_path;
    {
        std::ifstream groups { "/proc/self/cgroup" };
        std::string line;
        while (std::getline(groups, line)) {
            constexpr std::string_view unified_prefix = "0::";
            if (!line.starts_with(unified_prefix))
                continue;
            auto relative = line.substr(unified_prefix.size());
            while (!relative.empty() && relative.front() == '/')
                relative.erase(relative.begin());
            cgroup_path = std::filesystem::path { "/sys/fs/cgroup" };
            if (!relative.empty())
                cgroup_path /= relative;
            break;
        }
    }

    const auto read_first =
        [](const std::array<std::filesystem::path, 3>& paths)
        -> std::optional<std::uint64_t> {
        for (const auto& path : paths) {
            if (path.empty())
                continue;
            if (const auto value = read_decimal_file(path))
                return value;
        }
        return std::nullopt;
    };
    const std::array<std::filesystem::path, 3> limit_paths {
        cgroup_path.empty() ? std::filesystem::path { }
                            : cgroup_path / "memory.max",
        cgroup_path.empty() ? std::filesystem::path { }
                            : cgroup_path / "memory.limit_in_bytes",
        std::filesystem::path { "/sys/fs/cgroup/memory.max" },
    };
    const std::array<std::filesystem::path, 3> current_paths {
        cgroup_path.empty() ? std::filesystem::path { }
                            : cgroup_path / "memory.current",
        cgroup_path.empty() ? std::filesystem::path { }
                            : cgroup_path / "memory.usage_in_bytes",
        std::filesystem::path { "/sys/fs/cgroup/memory.current" },
    };
    if (const auto limit = read_first(limit_paths)) {
        snapshot.cgroup_limit_bytes = *limit;
        snapshot.cgroup_limit_known = true;
    }
    if (const auto current = read_first(current_paths)) {
        snapshot.cgroup_current_bytes = *current;
        snapshot.cgroup_current_known = true;
    }
    // cgroup-v1 uses a very large sentinel for "unlimited". Treat any limit
    // many times larger than physical memory as equivalent to no finite limit.
    if (snapshot.cgroup_limit_known && snapshot.physical_known &&
        snapshot.cgroup_limit_bytes != 0U && snapshot.physical_bytes != 0U &&
        snapshot.physical_bytes <=
            std::numeric_limits<std::uint64_t>::max() / 2U &&
        snapshot.cgroup_limit_bytes >
            snapshot.physical_bytes * std::uint64_t { 2U }) {
        snapshot.cgroup_limit_bytes = 0U;
        snapshot.cgroup_limit_known = false;
    }
#endif
    return snapshot;
}

} // namespace ilemu
