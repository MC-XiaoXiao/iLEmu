/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/factory.hpp"
#include "execution/code_memory.hpp"
#include <cstddef>
#include <memory>
namespace ilemu::execution {
namespace arm64 { class CodeCache; struct CompilationKey; struct PublishedTrace; }
// Immutable published translations using one allocator owned by the cache.
// No guest backing is retained. Local heat/recency and instruction leases stay
// independent; local entries retain code even after shared-cache eviction.
class SharedCompilationCache final : public SharedCacheResource {
public:
    explicit SharedCompilationCache(std::shared_ptr<CodeAllocator>,
        std::size_t budget = 16U * 1024U * 1024U);
    ~SharedCompilationCache() override;
    std::uint64_t retained_bytes() const noexcept override;
    void set_pressure_limited(bool) override;
    void clear();
private:
    friend class arm64::CodeCache;
    bool owns(const CodeAllocator&) const noexcept;
    std::shared_ptr<const arm64::PublishedTrace> find(const arm64::CompilationKey&);
    void insert(const arm64::CompilationKey&,
        std::shared_ptr<const arm64::PublishedTrace>);
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
