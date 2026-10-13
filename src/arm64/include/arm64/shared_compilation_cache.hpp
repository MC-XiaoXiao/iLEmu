/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/factory.hpp"
#include <cstddef>
#include <memory>
namespace ilemu::execution {
namespace arm64 { class CodeCache; struct CompilationKey; struct CompiledTrace; }
// Immutable translation plans, independent of host allocation and guest backing.
// Executable publication and mutable heat/recency remain local to each executor.
class SharedCompilationCache final : public SharedCacheResource {
public:
    explicit SharedCompilationCache(std::size_t budget = 16U * 1024U * 1024U);
    ~SharedCompilationCache() override;
    std::uint64_t retained_bytes() const noexcept override;
    void set_pressure_limited(bool) override;
    void clear();
private:
    friend class arm64::CodeCache;
    std::shared_ptr<const arm64::CompiledTrace> find(const arm64::CompilationKey&);
    void insert(const arm64::CompilationKey&,
        std::shared_ptr<const arm64::CompiledTrace>);
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
