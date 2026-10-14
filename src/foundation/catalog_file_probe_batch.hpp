/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "foundation/executable_catalog.hpp"
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <vector>
namespace ilemu {
class HostResourceController;
class HostWorkToken;
}
namespace ilemu::catalog_detail {
struct FilePrefix {
    std::array<std::byte, 16> bytes { };
    std::size_t size { };
};
struct FileProbe {
    FilePrefix prefix;
    ExecutableCatalogFileGeneration generation;
};
// Cache only bounded byte probes; catalog parsing/publication stays ordered.
class FileProbeBatch {
public:
    static constexpr std::size_t capacity = 128;
    using Read
        = std::function<std::optional<FileProbe>(const std::filesystem::path&)>;
    using Generation
        = std::function<std::optional<ExecutableCatalogFileGeneration>(
            const std::filesystem::path&)>;
    FileProbeBatch(Read, Generation);
    ~FileProbeBatch();
    // Borrowed paths must outlive this batch and its outstanding work.
    void prepare(std::span<const std::filesystem::path* const>);
    std::optional<FilePrefix> read(std::size_t, const std::filesystem::path&);

private:
    using Clock = std::chrono::steady_clock;
    struct Slot {
        const std::filesystem::path* path { };
        std::optional<FileProbe> probe;
        std::shared_ptr<HostWorkToken> token;
        Clock::duration elapsed { };
    };
    void observe(Clock::duration);
    Read read_;
    Generation generation_;
    std::vector<Slot> slots_;
    // Join work before the borrowed paths/callbacks/slots can expire.
    std::unique_ptr<HostResourceController> workers_;
    Clock::duration sampled_ { };
    unsigned samples_ { };
    bool parallel_ { };
    bool attempted_ { };
};
}
