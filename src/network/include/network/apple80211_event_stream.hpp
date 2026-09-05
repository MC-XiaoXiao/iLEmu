#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string_view>
#include <vector>

namespace ilemu::darwin::network::apple80211_driver {

enum class EventStreamProfile : std::uint8_t {
    Undetected,
    LegacyBitmask16,
    FramedRecords,
};

// /dev/airport changed from a 16-bit event mask to framed records while
// retaining the same device path.  The fixed buffer size used by the native
// reader is the protocol negotiation boundary, so each open description
// selects its profile on the first read and keeps it thereafter.
class EventStream {
public:
    void enqueue(std::uint32_t event);

    [[nodiscard]] bool readable() const;
    [[nodiscard]] std::uint32_t pending_byte_count() const;
    [[nodiscard]] EventStreamProfile profile() const { return profile_; }
    [[nodiscard]] std::string_view profile_name() const;

    [[nodiscard]] std::span<const std::byte> prepare_read(std::size_t capacity);
    void consume(std::size_t bytes);

private:
    void prepare_record();

    EventStreamProfile profile_ { EventStreamProfile::Undetected };
    std::deque<std::uint32_t> events_;
    std::vector<std::byte> record_;
    std::size_t record_offset_ { };
    std::size_t record_event_count_ { };
};

} // namespace ilemu::darwin::network::apple80211_driver
