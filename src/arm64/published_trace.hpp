/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "trace_metadata.hpp"
#include "execution/code_memory.hpp"
#include <memory>
namespace ilemu::execution::arm64 {
// No guest backing, source, mutable heat or native invocation state is retained.
// Const ownership prevents republishing; each local entry keeps execution alive
// independently of shared-cache eviction or another executor's invalidation.
struct PublishedTrace : TraceMetadata {
    std::shared_ptr<const ExecutableCode> code;
    std::size_t emitted_bytes = 0;
    std::size_t capacity() const noexcept { return code ? code->capacity() : 0; }
};
}
