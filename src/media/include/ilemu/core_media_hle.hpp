#pragma once

namespace ilemu {

class UserlandHleRegistry;

// CoreMedia asks its session manager for the current hardware audio route
// while MediaToolbox brings up the remote player service. The emulated device
// has no physical route, so this adapter returns the firmware's supported
// empty-route result and leaves all other media logic in the guest.
class CoreMediaHle {
public:
    explicit CoreMediaHle(UserlandHleRegistry& registry);

private:
    static void copy_device_route_for_audio_category(
        class UserlandHleCall& call);
};

} // namespace ilemu
