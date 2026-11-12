// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "filesystem/hfs_quota.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace ilemu::hfs {
namespace {
    constexpr std::uint32_t invalid = 22, io_error = 5, quota_exceeded = 69;
    constexpr std::uint32_t grace = 7 * 24 * 60 * 60;
    std::uint64_t get(const QuotaRecord& r, unsigned at, unsigned size = 4,
        bool big = false)
    {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i)
            value |= std::uint64_t(std::to_integer<unsigned>(r[at + i]))
                     << (8 * (big ? size - i - 1 : i));
        return value;
    }
    void put(QuotaRecord& r, unsigned at, std::uint64_t value,
        unsigned size = 4, bool big = false)
    {
        for (unsigned i = 0; i < size; ++i)
            r[at + i] = std::byte(value >> (8 * (big ? size - i - 1 : i)));
    }
    QuotaRecord endian(const QuotaRecord& r)
    {
        auto result = r;
        for (unsigned at = 0; at < 64;) {
            const unsigned size = at < 24 ? 8 : 4;
            std::reverse(result.begin() + at, result.begin() + at + size);
            at += size;
        }
        return result;
    }
    std::uint32_t host_error()
    {
        switch (errno) {
        case ENOENT: return 2;
        case EACCES: return 13;
        case EPERM: return 1;
        case ENOTDIR: return 20;
        case EISDIR: return 21;
        case EINVAL: return 22;
        case EMFILE: return 24;
        case ENAMETOOLONG: return 63;
        case EROFS: return 30;
        case ENOSPC: return 28;
        default: return io_error;
        }
    }
    struct Entry { QuotaRecord record {}; std::uint32_t index {}; };
    struct File {
        int fd {-1};
        QuotaRecord header {};
        std::uint32_t count {}, max {}, btime {}, itime {};
        std::map<std::uint32_t, Entry> entries;
        bool initialized {false};
        ~File()
        {
            if (fd >= 0) {
                if (initialized) static_cast<void>(sync());
                ::close(fd);
            }
        }
        std::uint32_t lookup(std::uint32_t id, Entry*& entry)
        {
            if (const auto found = entries.find(id); found != entries.end()) {
                entry = &found->second;
                return 0;
            }
            const auto mask = max - 1;
            const auto hash = id * 2654435761U;
            const auto shift = 32U - static_cast<unsigned>(std::countr_zero(max));
            const auto first = max == 1 ? 0U : (hash >> shift) & mask;
            const auto skip = ((hash >> 11U) & (mask >> 1U)) | 1U;
            for (std::uint32_t n = 0, index = first; n < max;
                 ++n, index = (index + skip) & mask) {
                QuotaRecord disk;
                if (::pread(fd, disk.data(), disk.size(), 64ULL * (index + 1)) != 64)
                    return io_error;
                const auto disk_id = get(disk, 44, 4, true);
                if (disk_id != 0 && disk_id != id)
                    continue;
                auto record = endian(disk);
                if (disk_id == 0) {
                    record = {};
                    put(record, 44, id);
                    disk = endian(record);
                    if (::pwrite(fd, disk.data(), disk.size(), 64ULL * (index + 1)) != 64)
                        return host_error();
                    ++count;
                }
                entry = &entries.emplace(id, Entry {record, index}).first->second;
                return 0;
            }
            return 68; // EUSERS: quota hash table is full.
        }
        std::uint32_t sync()
        {
            for (const auto& [id, e] : entries) {
                const auto disk = endian(e.record);
                if (::pwrite(fd, disk.data(), disk.size(), 64ULL * (e.index + 1)) != 64)
                    return host_error();
            }
            put(header, 12, count, 4, true);
            if (::pwrite(fd, header.data(), header.size(), 0) != 64 || ::fsync(fd) != 0)
                return host_error();
            return 0;
        }
    };
    void update_grace(QuotaRecord& next, const QuotaRecord& old,
        const File& file, std::uint32_t now)
    {
        if (get(next, 8, 8) && get(next, 16, 8) >= get(next, 8, 8) &&
            (!get(old, 8, 8) || get(old, 16, 8) < get(old, 8, 8)))
            put(next, 36, now + file.btime);
        if (get(next, 28) && get(next, 32) >= get(next, 28) &&
            (!get(old, 28) || get(old, 32) < get(old, 28)))
            put(next, 40, now + file.itime);
    }
}

struct Quotas::State {
    std::mutex mutex;
    std::map<std::string, std::array<std::unique_ptr<File>, 2>, std::less<>> mounts;
};
Quotas::Quotas() : state_(std::make_shared<State>()) {}
bool Quotas::active(std::string_view mount) const
{
    const std::lock_guard lock(state_->mutex);
    const auto found = state_->mounts.find(mount);
    return found != state_->mounts.end() && (found->second[0] || found->second[1]);
}
std::uint32_t Quotas::control(std::string_view mount, std::uint32_t command,
    std::uint32_t type, std::uint32_t id, QuotaRecord& record,
    const std::filesystem::path& path, std::uint32_t now)
{
    const std::lock_guard lock(state_->mutex);
    auto& files = state_->mounts[std::string(mount)];
    auto& file = files[type];
    if (command == 0x700) {
        put(record, 0, file ? 1 : 0);
        return 0;
    }
    if (command == 0x600) {
        for (const auto& f : files)
            if (f) { if (const auto e = f->sync()) return e; }
        return 0;
    }
    if (command == 0x200) {
        if (file) { if (const auto e = file->sync()) return e; }
        if (file) file->initialized = false;
        file.reset();
        return 0;
    }
    if (command == 0x100) {
        if (file) { if (const auto e = file->sync()) return e; }
        auto next = std::make_unique<File>();
        next->fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (next->fd < 0) return host_error();
        struct stat st {};
        if (::fstat(next->fd, &st) != 0) return host_error();
        if (!S_ISREG(st.st_mode)) return 13;
        if (::pread(next->fd, next->header.data(), 64, 0) != 64) return invalid;
        next->max = static_cast<std::uint32_t>(get(next->header, 8, 4, true));
        if (get(next->header, 0, 4, true) != (type ? 0xff31ff27U : 0xff31ff35U) ||
            get(next->header, 4, 4, true) > 1 || !std::has_single_bit(next->max) ||
            64ULL * (next->max + 1ULL) > std::uint64_t(st.st_size)) return invalid;
        next->count = static_cast<std::uint32_t>(get(next->header, 12, 4, true));
        next->btime = static_cast<std::uint32_t>(get(next->header, 24, 4, true));
        next->itime = static_cast<std::uint32_t>(get(next->header, 28, 4, true));
        if (!next->btime) next->btime = grace;
        if (!next->itime) next->itime = grace;
        next->initialized = true;
        if (file) file->initialized = false;
        file = std::move(next);
        return 0;
    }
    if (command != 0x300 && command != 0x400 && command != 0x500) return invalid;
    if (!file || id == 0) return invalid;
    Entry* entry;
    if (const auto error = file->lookup(id, entry)) return error;
    if (command == 0x300) {
        record = entry->record;
        return 0;
    }
    auto next = entry->record;
    if (command == 0x400) {
        put(next, 0, get(record, 0, 8), 8);
        put(next, 8, get(record, 8, 8), 8);
        put(next, 24, get(record, 24));
        put(next, 28, get(record, 28));
    } else {
        put(next, 16, get(record, 16, 8), 8);
        put(next, 32, get(record, 32));
    }
    update_grace(next, entry->record, *file, now);
    entry->record = next;
    return 0;
}
std::uint32_t Quotas::change(std::string_view mount, std::uint32_t owner,
    std::uint32_t group, std::int64_t bytes, std::int64_t inodes,
    bool privileged, std::uint32_t now)
{
    const std::lock_guard lock(state_->mutex);
    const auto found = state_->mounts.find(mount);
    if (found == state_->mounts.end()) return 0;
    std::array<Entry*, 2> entries {};
    std::array<QuotaRecord, 2> next {};
    for (unsigned type = 0; type != 2; ++type) {
        const auto& file = found->second[type];
        const auto id = type ? group : owner;
        if (!file || id == 0) continue;
        if (const auto error = file->lookup(id, entries[type])) return error;
        const auto& old = entries[type]->record;
        auto& r = next[type] = old;
        const auto add = [](std::uint64_t value, std::int64_t delta) -> std::uint64_t {
            return delta < 0 ? value - std::min(value, std::uint64_t(-delta))
                             : value + static_cast<std::uint64_t>(delta);
        };
        put(r, 16, add(get(old, 16, 8), bytes), 8);
        put(r, 32, add(get(old, 32), inodes));
        if (!privileged &&
            ((bytes > 0 && ((get(r, 0, 8) && get(r, 16, 8) >= get(r, 0, 8)) ||
                (get(r, 8, 8) && get(r, 16, 8) >= get(r, 8, 8) &&
                    get(old, 16, 8) >= get(old, 8, 8) && now > get(old, 36)))) ||
             (inodes > 0 && ((get(r, 24) && get(r, 32) >= get(r, 24)) ||
                (get(r, 28) && get(r, 32) >= get(r, 28) &&
                    get(old, 32) >= get(old, 28) && now > get(old, 40))))))
            return quota_exceeded;
        update_grace(r, old, *file, now);
    }
    for (unsigned type = 0; type != 2; ++type)
        if (entries[type]) entries[type]->record = next[type];
    return 0;
}
} // namespace ilemu::hfs
