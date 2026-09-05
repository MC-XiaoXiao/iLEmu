#pragma once

#include <memory>
#include <string>

#include "foundation/host_memory.hpp"

namespace ilemu {

class AudioDecoder;
class AudioSink;
class ControlChannel;
class DisplayPresenter;
struct DeviceProfile;

struct SessionAudio {
    std::shared_ptr<AudioSink> sink;
    std::shared_ptr<AudioDecoder> decoder;
    std::string backend_name { "none" };
    std::string decoder_name { "pcm-caf-only" };
};

// Host services are supplied by the embedding frontend. No native window,
// audio, terminal descriptor or operating-system SDK crosses this boundary.
class SessionHost {
public:
    virtual ~SessionHost() = default;
    virtual void initialize_graphics() = 0;
    [[nodiscard]] virtual std::unique_ptr<DisplayPresenter> create_display(
        const DeviceProfile& device) = 0;
    [[nodiscard]] virtual std::unique_ptr<ControlChannel> create_control(
        const DeviceProfile& device) = 0;
    [[nodiscard]] virtual SessionAudio create_audio() = 0;
    [[nodiscard]] virtual HostMemorySnapshot memory_snapshot() const = 0;
    [[nodiscard]] virtual HostMemoryBudgetSnapshot
    memory_budget_snapshot() const = 0;
};

} // namespace ilemu
