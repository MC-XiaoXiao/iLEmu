/* SPDX-License-Identifier: MPL-2.0 */
#include "code_pool.hpp"
#if defined(__linux__)
#include <oaknut/dual_code_block.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>

namespace ilemu::host::detail {
namespace {
    constexpr unsigned slots_per_chunk = 16;
    constexpr unsigned full_chunk = (1U << slots_per_chunk) - 1;

    struct Chunk {
        explicit Chunk(std::size_t capacity)
            : memory(capacity * slots_per_chunk)
        {
        }
        oaknut::DualCodeBlock memory;
        unsigned used = 0;
        unsigned discard_threshold = 0;
    };

    class Pool;
    class PooledCode final : public execution::ExecutableCode {
    public:
        PooledCode(std::shared_ptr<Pool>, std::shared_ptr<Chunk>, unsigned,
            std::size_t);
        ~PooledCode() override;
        std::size_t capacity() const noexcept override { return capacity_; }
        const void* entry() const noexcept override;
        void publish(std::span<const std::byte>) override;

    private:
        std::shared_ptr<Pool> pool_;
        std::shared_ptr<Chunk> chunk_;
        unsigned slot_;
        std::size_t capacity_;
        bool published_ = false;
    };

    // Slot capacities preserve cache accounting. Reuse any vacant slot before
    // growing; retire empty chunks, keeping at most one for the next refill.
    // Vacant OS pages can be discarded without moving live code. Code objects
    // own the pool so an allocator can be destroyed before its allocations.
    class Pool final : public std::enable_shared_from_this<Pool> {
    public:
        Pool(std::shared_ptr<execution::CodeAllocator> fallback,
            std::size_t capacity, std::size_t page)
            : fallback_(std::move(fallback)), capacity_(capacity), page_(page)
        {
        }
        std::size_t capacity() const noexcept { return capacity_; }
        std::unique_ptr<execution::ExecutableCode> allocate(std::size_t bytes)
        {
            if (bytes == 0 || bytes > capacity_)
                return fallback_->allocate(bytes);
            std::unique_lock lock(mutex_);
            auto available = std::find_if(chunks_.begin(), chunks_.end(),
                [](const auto& chunk) { return chunk->used != full_chunk; });
            if (available == chunks_.end() && !unavailable_) {
                try {
                    chunks_.push_back(std::make_shared<Chunk>(capacity_));
                    available = std::prev(chunks_.end());
                } catch (const std::bad_alloc&) {
                    // Host policy or unavailable memfd support must not prevent
                    // the existing anonymous-map publication path from working.
                    unavailable_ = true;
                }
            }
            if (available == chunks_.end()) {
                lock.unlock();
                return fallback_->allocate(bytes);
            }
            auto& chunk = *available;
            const auto slot = static_cast<unsigned>(std::countr_one(chunk->used));
            auto code = std::make_unique<PooledCode>(
                shared_from_this(), chunk, slot, capacity_);
            chunk->used |= 1U << slot;
            const auto live = static_cast<unsigned>(std::popcount(chunk->used));
            // Rearm after meaningful growth, rather than repeatedly discarding
            // the same page during single-neighbor publication/reuse.
            if (live >= slots_per_chunk / 4)
                chunk->discard_threshold = std::max(
                    chunk->discard_threshold, std::bit_floor(live / 2));
            return code;
        }
        void release(const std::shared_ptr<Chunk>& chunk, unsigned slot)
        {
            std::lock_guard lock(mutex_);
            chunk->used &= ~(1U << slot);
            if (chunk->used == 0) {
                if (chunks_.size() > 1)
                    std::erase(chunks_, chunk);
                return;
            }
            const auto live = static_cast<unsigned>(std::popcount(chunk->used));
            if (live <= chunk->discard_threshold) {
                discard_unused_pages(*chunk);
                chunk->discard_threshold = live / 2;
            }
        }

    private:
        void discard_unused_pages(Chunk& chunk)
        {
            if (discard_unavailable_)
                return;
            const auto extent = capacity_ * slots_per_chunk;
            std::size_t begin = 0, length = 0;
            bool succeeded = true;
            const auto discard = [&] {
                if (length == 0)
                    return;
                auto* address =
                    reinterpret_cast<std::byte*>(chunk.memory.wptr()) + begin;
                // Removing shared backing also drops its executable alias.
                // The mutex excludes new claims; every slot touching this
                // complete OS-page range is vacant, including unpublished code.
                if (::madvise(address, length, MADV_REMOVE) != 0) {
                    succeeded = false;
                    discard_unavailable_ = errno == EOPNOTSUPP ||
                        errno == ENOSYS || errno == EINVAL ||
                        errno == EACCES || errno == EPERM;
                }
                length = 0;
            };
            for (std::size_t offset = 0;
                succeeded && offset + page_ <= extent; offset += page_) {
                const auto first = offset / capacity_;
                const auto last = (offset + page_ - 1) / capacity_;
                const auto owners = ((1U << (last - first + 1)) - 1) << first;
                if ((chunk.used & owners) == 0) {
                    if (length == 0)
                        begin = offset;
                    length += page_;
                } else {
                    discard();
                }
            }
            if (succeeded)
                discard();
        }
        std::shared_ptr<execution::CodeAllocator> fallback_;
        const std::size_t capacity_, page_;
        std::mutex mutex_;
        std::vector<std::shared_ptr<Chunk>> chunks_;
        bool unavailable_ = false, discard_unavailable_ = false;
    };

    PooledCode::PooledCode(std::shared_ptr<Pool> pool,
        std::shared_ptr<Chunk> chunk, unsigned slot, std::size_t capacity)
        : pool_(std::move(pool)), chunk_(std::move(chunk)), slot_(slot),
          capacity_(capacity)
    {
    }
    PooledCode::~PooledCode() { pool_->release(chunk_, slot_); }
    const void* PooledCode::entry() const noexcept
    {
        return published_
            ? reinterpret_cast<const std::byte*>(chunk_->memory.xptr()) +
                  slot_ * capacity_
            : nullptr;
    }
    void PooledCode::publish(std::span<const std::byte> bytes)
    {
        if (published_ || bytes.empty() || bytes.size() > capacity_)
            throw std::logic_error("invalid code publication");
        auto* writable = reinterpret_cast<char*>(chunk_->memory.wptr()) +
                         slot_ * capacity_;
        auto* executable = reinterpret_cast<char*>(chunk_->memory.xptr()) +
                           slot_ * capacity_;
        std::memcpy(writable, bytes.data(), bytes.size());
        // Clean the written alias before invalidating the execution alias.
        // Neither mapping changes permissions while other slots are executing.
        __builtin___clear_cache(writable, writable + bytes.size());
        __builtin___clear_cache(executable, executable + bytes.size());
        published_ = true;
    }

    class PooledAllocator final : public execution::CodeAllocator {
    public:
        PooledAllocator(std::unique_ptr<execution::CodeAllocator> fallback,
            std::size_t page)
            : fallback_(std::move(fallback)), page_(page)
        {
            if (!fallback_ || page < slots_per_chunk || !std::has_single_bit(page))
                throw std::invalid_argument("invalid executable pool geometry");
            const auto host_page = ::sysconf(_SC_PAGESIZE);
            if (host_page <= 0)
                throw std::runtime_error("host page size is unavailable");
            // Sixteen minimum slots occupy one system page. Larger size classes
            // reuse the same pool; only the slot stride and chunk length differ.
            for (std::size_t i = 0; i < pools_.size(); ++i)
                pools_[i] = std::make_shared<Pool>(
                    fallback_, (page / slots_per_chunk) << i,
                    static_cast<std::size_t>(host_page));
        }
        std::unique_ptr<execution::ExecutableCode> allocate(
            std::size_t bytes) override
        {
            if (bytes == 0 || bytes > page_)
                return fallback_->allocate(bytes);
            const auto pool = std::find_if(pools_.begin(), pools_.end(),
                [bytes](const auto& candidate) {
                    return bytes <= candidate->capacity();
                });
            return (*pool)->allocate(bytes);
        }

    private:
        std::shared_ptr<execution::CodeAllocator> fallback_;
        const std::size_t page_;
        std::array<std::shared_ptr<Pool>, std::bit_width(slots_per_chunk)> pools_;
    };
}
std::unique_ptr<execution::CodeAllocator> make_pooled_code_allocator(
    std::unique_ptr<execution::CodeAllocator> fallback, std::size_t page_size)
{
    return std::make_unique<PooledAllocator>(std::move(fallback), page_size);
}
}
#endif
