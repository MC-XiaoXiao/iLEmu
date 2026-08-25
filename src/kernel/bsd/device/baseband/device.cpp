#include "ilemu/baseband_device.hpp"

#include "ilemu/darwin_tty_abi.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ilemu::bsd::baseband_device {

namespace {

    std::optional<std::uint32_t> numeric_suffix(std::string_view candidate)
    {
        constexpr std::array<std::string_view, 2> prefixes {
            "/dev/dlci.spi-baseband.", "/dev/dlci.h5.baseband."
        };
        for (const auto prefix : prefixes) {
            if (!candidate.starts_with(prefix)) {
                continue;
            }
            const auto suffix = candidate.substr(prefix.size());
            if (suffix.empty()) {
                return std::nullopt;
            }
            std::uint32_t value { };
            const auto result = std::from_chars(
                suffix.data(), suffix.data() + suffix.size(), value, 10);
            if (result.ec != std::errc { } ||
                result.ptr != suffix.data() + suffix.size() || value == 0) {
                return std::nullopt;
            }
            return value;
        }
        return std::nullopt;
    }

    std::uint32_t channel_for_path(std::string_view candidate)
    {
        return numeric_suffix(candidate).value_or(0U);
    }

} // namespace

std::shared_ptr<OpenDescription> State::open_description(
    std::string_view candidate, std::uint32_t process_id)
{
    const auto channel = channel_for_path(candidate);
    const auto is_channel = is_mux_channel_path(candidate);
    const std::lock_guard lock { mutex_ };
    if (!available_ || exclusive_owner_.has_value() ||
        (is_channel && (!dynamic_channels_available_ ||
                           (anonymous_mux_channel_capacity_ != 0 &&
                               channel > anonymous_mux_channel_capacity_)))) {
        return { };
    }
    channels_.try_emplace(channel);
    const auto token = next_open_token_++;
    return std::shared_ptr<OpenDescription>(
        new OpenDescription { lifetime_, channel, token, process_id });
}

OpenDescription::~OpenDescription()
{
    if (lifetime_ && lifetime_->state)
        lifetime_->state->release_description(*this);
}

std::vector<std::byte> OpenDescription::receive(std::size_t maximum) const
{
    return lifetime_ && lifetime_->state
               ? lifetime_->state->receive_channel(channel_, maximum)
               : std::vector<std::byte> { };
}

std::size_t OpenDescription::pending_receive_bytes() const
{
    return lifetime_ && lifetime_->state
               ? lifetime_->state->pending_receive_channel(channel_)
               : 0U;
}

bool OpenDescription::receive_eof() const
{
    return lifetime_ && lifetime_->state
               ? lifetime_->state->receive_eof_channel(channel_)
               : true;
}

std::size_t OpenDescription::write(std::span<const std::byte> bytes) const
{
    return lifetime_ && lifetime_->state
               ? lifetime_->state->write_channel(channel_, bytes)
               : 0U;
}

bool OpenDescription::writable() const
{
    return lifetime_ && lifetime_->state
               ? lifetime_->state->writable_channel(channel_)
               : false;
}

bool OpenDescription::transmit_sink_failed() const
{
    return lifetime_ && lifetime_->state
               ? lifetime_->state->sink_failed_channel(channel_)
               : true;
}

IoctlResult OpenDescription::ioctl(std::uint32_t command) const
{
    return lifetime_ && lifetime_->state
               ? lifetime_->state->ioctl_for_owner(command, token_, process_id_)
               : IoctlResult::unsupported;
}

void OpenDescription::flush_buffers(std::uint32_t what) const
{
    if (lifetime_ && lifetime_->state)
        lifetime_->state->flush_channel(channel_, what);
}

bool State::available() const
{
    const std::lock_guard lock { mutex_ };
    return available_;
}

void State::set_available(bool available)
{
    const std::lock_guard lock { mutex_ };
    available_ = available;
}

bool State::transmit_queue_writable() const
{
    const std::lock_guard lock { mutex_ };
    return transmit_queue_writable_ && !transmit_sink_failed_;
}

void State::set_transmit_queue_writable(bool writable)
{
    const std::lock_guard lock { mutex_ };
    transmit_queue_writable_ = writable;
}

bool State::dynamic_channels_available() const
{
    const std::lock_guard lock { mutex_ };
    return dynamic_channels_available_;
}

void State::set_dynamic_channels_available(bool available)
{
    const std::lock_guard lock { mutex_ };
    dynamic_channels_available_ = available;
}

bool State::mux_channel_path_available(std::string_view candidate) const
{
    const auto unit = numeric_suffix(candidate);
    if (!unit) {
        return false;
    }
    const std::lock_guard lock { mutex_ };
    if (!dynamic_channels_available_) {
        return false;
    }
    return anonymous_mux_channel_capacity_ == 0 ||
           *unit <= anonymous_mux_channel_capacity_;
}

bool State::may_open(bool privileged) const
{
    static_cast<void>(privileged);
    const std::lock_guard lock { mutex_ };
    return available_ && !exclusive_owner_.has_value();
}

IoctlResult State::ioctl(std::uint32_t command)
{
    return ioctl_for_owner(command, 0U, 0U);
}

IoctlResult State::ioctl_for_owner(
    std::uint32_t command, std::uint64_t token, std::uint32_t process_id)
{
    const std::lock_guard lock { mutex_ };
    switch (command) {
    case darwin::tty::set_exclusive:
        if (exclusive_owner_ &&
            (exclusive_owner_->token != token ||
                exclusive_owner_->process_id != process_id)) {
            return IoctlResult::permission_denied;
        }
        exclusive_ = true;
        exclusive_owner_ = OpenOwner { token, process_id };
        return IoctlResult::success;
    case darwin::tty::clear_exclusive:
        if (token != 0U &&
            (!exclusive_owner_ || exclusive_owner_->token != token ||
                exclusive_owner_->process_id != process_id)) {
            return IoctlResult::permission_denied;
        }
        exclusive_ = false;
        exclusive_owner_.reset();
        return IoctlResult::success;
    default:
        return IoctlResult::unsupported;
    }
}

bool State::exclusive() const
{
    const std::lock_guard lock { mutex_ };
    return exclusive_;
}

darwin::tty::Arm32Attributes State::attributes() const
{
    const std::lock_guard lock { mutex_ };
    return attributes_;
}

void State::set_attributes(const darwin::tty::Arm32Attributes& attributes)
{
    const std::lock_guard lock { mutex_ };
    attributes_ = attributes;
    channels_[0U].minimum_receive_bytes = std::min<std::size_t>(
        attributes.control_characters[darwin::tty::minimum_bytes_index],
        maximum_receive_threshold);
}

bool State::receive_eof() const
{
    const std::lock_guard lock { mutex_ };
    return receive_eof_;
}

void State::set_receive_eof(bool eof)
{
    const std::lock_guard lock { mutex_ };
    receive_eof_ = eof;
}

bool State::h5_transport_mode() const
{
    const std::lock_guard lock { mutex_ };
    return h5_transport_mode_;
}

void State::set_h5_transport_mode(bool enabled)
{
    const std::lock_guard lock { mutex_ };
    h5_transport_mode_ = enabled;
}

std::size_t State::minimum_receive_bytes() const
{
    const std::lock_guard lock { mutex_ };
    const auto channel = channels_.find(0U);
    return channel == channels_.end() ? 0U
                                      : channel->second.minimum_receive_bytes;
}

void State::set_minimum_receive_bytes(std::size_t bytes)
{
    const std::lock_guard lock { mutex_ };
    const auto bounded = std::min(bytes, maximum_receive_threshold);
    channels_[0U].minimum_receive_bytes = bounded;
    attributes_.control_characters[darwin::tty::minimum_bytes_index] =
        static_cast<std::uint8_t>(std::min<std::size_t>(bounded, 0xffU));
}

std::uint32_t State::modem_control_bits() const
{
    const std::lock_guard lock { mutex_ };
    return modem_control_bits_;
}

void State::set_modem_control_bits(std::uint32_t bits)
{
    const std::lock_guard lock { mutex_ };
    modem_control_bits_ = bits;
}

void State::update_modem_control_bits(std::uint32_t bits, bool enabled)
{
    const std::lock_guard lock { mutex_ };
    if (enabled) {
        modem_control_bits_ |= bits;
    } else {
        modem_control_bits_ &= ~bits;
    }
}

bool State::configure_receive_queue(std::span<const std::byte> configuration)
{
    if (configuration.size() != receive_queue_configuration_.size()) {
        return false;
    }
    const std::lock_guard lock { mutex_ };
    std::copy(configuration.begin(), configuration.end(),
        receive_queue_configuration_.begin());
    receive_queue_configured_ = true;
    return true;
}

bool State::receive_queue_configured() const
{
    const std::lock_guard lock { mutex_ };
    return receive_queue_configured_;
}

void State::flush_buffers(std::uint32_t what) { flush_channel(0U, what); }

void State::set_mux_channel_capacity(std::uint32_t capacity)
{
    const std::lock_guard lock { mutex_ };
    anonymous_mux_channel_capacity_ = capacity;
    next_anonymous_mux_channel_ = 1;
    // Keep named channels out of the anonymous slot range.  The normal boot
    // configures this before CommCenter opens the device, so changing a live
    // transport remains a safe administrative operation as well.
    if (capacity != 0 && next_mux_channel_ <= capacity) {
        next_mux_channel_ = capacity + 1U;
    }
}

std::uint32_t State::register_mux_channel(std::string_view name)
{
    const std::lock_guard lock { mutex_ };
    if (!name.empty()) {
        const auto key = std::string { name };
        if (const auto found = mux_channels_.find(key);
            found != mux_channels_.end()) {
            return found->second;
        }
        if (anonymous_mux_channel_capacity_ != 0 &&
            mux_channels_.size() >= anonymous_mux_channel_capacity_) {
            // Named channels use a separate ID range so they never alias the
            // anonymous slots. Keep their registry bounded as well; otherwise
            // an offline CommCenter retry loop could grow this map
            // indefinitely.
            return 0;
        }
        const auto unit = next_mux_channel_++;
        mux_channels_.emplace(key, unit);
        return unit;
    }
    if (anonymous_mux_channel_capacity_ != 0) {
        const auto unit = next_anonymous_mux_channel_;
        next_anonymous_mux_channel_ =
            unit == anonymous_mux_channel_capacity_ ? 1U : unit + 1U;
        return unit;
    }
    return next_mux_channel_++;
}

std::optional<std::uint32_t> State::mux_channel(std::string_view name) const
{
    const std::lock_guard lock { mutex_ };
    if (name.empty()) {
        return std::nullopt;
    }
    const auto found = mux_channels_.find(std::string { name });
    if (found == mux_channels_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void State::enqueue_receive(std::span<const std::byte> bytes)
{
    enqueue_receive(0U, bytes);
}

void State::enqueue_receive(
    std::uint32_t channel_number, std::span<const std::byte> bytes)
{
    const std::lock_guard lock { mutex_ };
    auto& channel = channels_[channel_number];
    channel.receive_queue.insert(
        channel.receive_queue.end(), bytes.begin(), bytes.end());
}

std::vector<std::byte> State::receive(std::size_t maximum)
{
    return receive_channel(0U, maximum);
}

std::vector<std::byte> State::receive_channel(
    std::uint32_t channel_number, std::size_t maximum)
{
    const std::lock_guard lock { mutex_ };
    const auto channel = channels_.find(channel_number);
    if (channel == channels_.end())
        return { };
    const auto& state = channel->second;
    if (state.minimum_receive_bytes != 0 &&
        state.receive_queue.size() < state.minimum_receive_bytes &&
        !receive_eof_) {
        return { };
    }
    const auto count = std::min(maximum, state.receive_queue.size());
    std::vector<std::byte> bytes;
    bytes.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        bytes.push_back(channel->second.receive_queue.front());
        channel->second.receive_queue.pop_front();
    }
    return bytes;
}

std::size_t State::pending_receive_bytes() const
{
    return pending_receive_channel(0U);
}

std::size_t State::pending_receive_channel(std::uint32_t channel_number) const
{
    const std::lock_guard lock { mutex_ };
    const auto channel = channels_.find(channel_number);
    if (channel == channels_.end())
        return 0U;
    if (channel->second.minimum_receive_bytes != 0 &&
        channel->second.receive_queue.size() <
            channel->second.minimum_receive_bytes &&
        !receive_eof_) {
        return 0U;
    }
    return channel->second.receive_queue.size();
}

bool State::receive_eof_channel(std::uint32_t channel_number) const
{
    const std::lock_guard lock { mutex_ };
    return receive_eof_ && channels_.contains(channel_number);
}

std::size_t State::write(std::span<const std::byte> bytes)
{
    return write_channel(0U, bytes);
}

std::size_t State::write_channel(
    std::uint32_t channel_number, std::span<const std::byte> bytes)
{
    const std::lock_guard lock { mutex_ };
    if (!transmit_queue_writable_ || transmit_sink_failed_)
        return 0U;
    // Logical DLCI endpoints are real bounded endpoints, but Offline has no
    // modem peer. Their successful writes terminate at this null sink and do
    // not share the fixed TTY's capture history.
    if (channel_number != 0U)
        return bytes.size();
    if (transmit_sink_) {
        if (!transmit_sink_(bytes)) {
            transmit_sink_failed_ = true;
            return 0;
        }
        return bytes.size();
    }
    if (!transmit_capture_enabled_)
        return bytes.size();
    // Retain only the newest diagnostic bytes. Device write semantics remain
    // synchronous and successful, while the in-memory inspection path cannot
    // grow with an offline CommCenter retry loop.
    if (bytes.size() >= transmit_capture_capacity) {
        transmitted_.assign(bytes.end() - static_cast<std::ptrdiff_t>(
                                              transmit_capture_capacity),
            bytes.end());
        return bytes.size();
    }
    const auto retained_room = transmit_capture_capacity - bytes.size();
    if (transmitted_.size() > retained_room) {
        transmitted_.erase(transmitted_.begin(),
            transmitted_.begin() + static_cast<std::ptrdiff_t>(
                                       transmitted_.size() - retained_room));
    }
    transmitted_.insert(transmitted_.end(), bytes.begin(), bytes.end());
    return bytes.size();
}

bool State::writable_channel(std::uint32_t channel_number) const
{
    static_cast<void>(channel_number);
    const std::lock_guard lock { mutex_ };
    return transmit_queue_writable_ && !transmit_sink_failed_;
}

bool State::sink_failed_channel(std::uint32_t channel_number) const
{
    const std::lock_guard lock { mutex_ };
    return channel_number == 0U && transmit_sink_failed_;
}

void State::flush_channel(std::uint32_t channel_number, std::uint32_t what)
{
    const std::lock_guard lock { mutex_ };
    // Darwin's TIOCFLUSH treats zero as both FREAD and FWRITE. The offline
    // endpoint has no asynchronous transmit queue; only its receive side can
    // contain bytes that need to be discarded here.
    if (what == 0 || (what & 0x1U) != 0)
        channels_[channel_number].receive_queue.clear();
}

void State::release_description(const OpenDescription& description)
{
    const std::lock_guard lock { mutex_ };
    if (exclusive_owner_ && exclusive_owner_->token == description.token_ &&
        exclusive_owner_->process_id == description.process_id_) {
        exclusive_owner_.reset();
        exclusive_ = false;
    }
}

std::vector<std::byte> State::take_transmitted()
{
    const std::lock_guard lock { mutex_ };
    auto bytes = std::move(transmitted_);
    transmitted_.clear();
    return bytes;
}

void State::set_transmit_capture_enabled(bool enabled)
{
    const std::lock_guard lock { mutex_ };
    transmit_capture_enabled_ = enabled;
    transmit_sink_ = { };
    transmit_sink_failed_ = false;
    if (!enabled)
        transmitted_.clear();
}

void State::set_transmit_sink(TransmitSink sink)
{
    const std::lock_guard lock { mutex_ };
    transmit_sink_ = std::move(sink);
    transmit_capture_enabled_ = false;
    transmit_sink_failed_ = false;
    transmitted_.clear();
}

bool State::transmit_sink_failed() const
{
    const std::lock_guard lock { mutex_ };
    return transmit_sink_failed_;
}

bool is_mux_channel_path(std::string_view candidate)
{
    return numeric_suffix(candidate).has_value();
}

bool is_mux_path(std::string_view candidate)
{
    return candidate == spi_mux_path || candidate == h5_mux_path ||
           is_mux_channel_path(candidate);
}

bool is_path(std::string_view candidate)
{
    return candidate == path || candidate == legacy_path ||
           is_mux_path(candidate);
}

} // namespace ilemu::bsd::baseband_device
