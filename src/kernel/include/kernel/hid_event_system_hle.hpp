#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ilemu {

class Cpu;
class UserlandHleCall;
class UserlandHleRegistry;
struct KernelSharedState;

class HidEventSystemHle {
public:
    explicit HidEventSystemHle(UserlandHleRegistry& registry);
    void set_shared_state(std::shared_ptr<KernelSharedState> state);
    void reset(std::uint32_t process);
    void prepare_pending_event(
        Cpu& cpu, std::uint32_t process, std::uint32_t svc_immediate);

private:
    UserlandHleRegistry& registry_;
    std::shared_ptr<KernelSharedState> state_;
    std::uint32_t consumer_process_ { };
    std::size_t consumer_processor_ { };
    bool delivering_ { };
};

} // namespace ilemu
