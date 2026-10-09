/* SPDX-License-Identifier: MPL-2.0 */
#include "host_execution/code_memory.hpp"
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace ilemu::host {
#if defined(__linux__)
namespace {
    class PosixCode final : public execution::ExecutableCode {
    public:
        explicit PosixCode(std::size_t bytes)
        {
            const auto page = ::sysconf(_SC_PAGESIZE);
            if (page <= 0 || bytes == 0 ||
                bytes > std::numeric_limits<std::size_t>::max() -
                            static_cast<std::size_t>(page))
                throw std::length_error("invalid executable code size");
            size_ = ((bytes + static_cast<std::size_t>(page) - 1) /
                        static_cast<std::size_t>(page)) *
                    static_cast<std::size_t>(page);
            data_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (data_ == MAP_FAILED)
                throw std::system_error(
                    errno, std::generic_category(), "allocate executable code");
        }
        ~PosixCode() override { ::munmap(data_, size_); }
        std::size_t capacity() const noexcept override { return size_; }
        const void* entry() const noexcept override
        {
            return published_ ? data_ : nullptr;
        }
        void publish(std::span<const std::byte> bytes) override
        {
            if (published_ || bytes.empty() || bytes.size() > size_)
                throw std::logic_error("invalid code publication");
            std::memcpy(data_, bytes.data(), bytes.size());
            auto* begin = static_cast<char*>(data_);
            __builtin___clear_cache(begin, begin + bytes.size());
            if (::mprotect(data_, size_, PROT_READ | PROT_EXEC) != 0)
                throw std::system_error(
                    errno, std::generic_category(), "publish executable code");
            published_ = true;
        }

    private:
        void* data_ = nullptr;
        std::size_t size_ = 0;
        bool published_ = false;
    };
    class PosixAllocator final : public execution::CodeAllocator {
    public:
        std::unique_ptr<execution::ExecutableCode> allocate(
            std::size_t bytes) override
        {
            return std::make_unique<PosixCode>(bytes);
        }
    };
}
#endif
std::unique_ptr<execution::CodeAllocator> make_code_allocator()
{
#if defined(__linux__)
    return std::make_unique<PosixAllocator>();
#else
    throw std::runtime_error("host executable code publication is unavailable");
#endif
}
}
