// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ilemu {

// Demand-backed storage shared by serial and per-executor A32 page views.
class JitPageTableStorage {
public:
    static constexpr std::size_t entry_count = std::size_t { 1 } << 20U;
    static constexpr std::size_t byte_size = entry_count * sizeof(std::uint8_t*);
    JitPageTableStorage();
    ~JitPageTableStorage();
    JitPageTableStorage(const JitPageTableStorage&) = delete;
    JitPageTableStorage& operator=(const JitPageTableStorage&) = delete;
    [[nodiscard]] std::uint8_t** entries() const;
    void clear();
private:
    void* mapping_ { };
};

} // namespace ilemu
