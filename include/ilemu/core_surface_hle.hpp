#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ilemu {

class AddressSpace;
class DisplayState;
class PresentationTracker;
class SceneCoordinator;
struct KernelSharedState;
class SurfaceStore;
class UserlandHleCall;
class UserlandHleRegistry;

// User-space replacement for CoreSurface's IOKit client-buffer transport.
// The public CoreSurfaceBuffer CFRuntime wrapper remains firmware code; only
// its _CoreSurfaceClientBuffer* backing operations are intercepted.
class CoreSurfaceHle {
  public:
    CoreSurfaceHle(UserlandHleRegistry& registry,
                   std::shared_ptr<DisplayState> display,
                   std::shared_ptr<SurfaceStore> surfaces = {},
                   std::shared_ptr<PresentationTracker> presentations = {});

    void reset();
    void inherit_state(const CoreSurfaceHle& parent);
    void set_display(std::shared_ptr<DisplayState> display);
    void set_presentation_tracker(
        std::shared_ptr<PresentationTracker> presentations);
    void set_shared_state(std::shared_ptr<KernelSharedState> shared_state);
    void set_scene_coordinator(std::shared_ptr<SceneCoordinator> scenes);
    // The display user client exposes its front buffer as a reserved
    // CoreSurface ID. Unlike ordinary buffers, firmware draws into it while
    // it remains locked and the display controller scans it out at vsync.
    // Return true only when a newly visible frame was submitted.
    bool refresh_default_scanout(AddressSpace& memory);

  private:
    struct Buffer {
        std::uint32_t client{};
        std::uint32_t id{};
        std::uint32_t base{};
        std::uint32_t allocation_size{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t bytes_per_row{};
        std::uint32_t pixel_format{};
        std::uint32_t references{1};
        bool owns_memory{};
        std::vector<std::uint32_t> preserved_exit_snapshot_pixels;
    };
    struct CreateRequest {
        std::uint32_t dictionary{};
        std::array<std::uint32_t, 7> properties{};
        std::size_t property_index{};
        std::uint32_t number_output{};
    };

    void dispatch(UserlandHleCall& call);
    void create_from_dictionary(UserlandHleCall& call,
                                std::uint32_t dictionary);
    void
    read_next_create_property(UserlandHleCall& call,
                              const std::shared_ptr<CreateRequest>& request);
    void finish_create_from_dictionary(
        UserlandHleCall& call, const std::shared_ptr<CreateRequest>& request);
    [[nodiscard]] std::uint32_t
    create_default_buffer(UserlandHleCall& call,
                          std::uint32_t requested_id = 0);
    [[nodiscard]] std::uint32_t wrap_client_memory(UserlandHleCall& call,
                                                   std::uint32_t base,
                                                   std::uint32_t size);
    [[nodiscard]] std::uint32_t
    create_buffer(UserlandHleCall& call, std::uint32_t base, std::uint32_t size,
                  std::uint32_t width, std::uint32_t height,
                  std::uint32_t bytes_per_row, std::uint32_t pixel_format,
                  bool owns_memory, std::uint32_t requested_id = 0,
                  bool publish = true);
    [[nodiscard]] Buffer* find(std::uint32_t client);
    void submit(Buffer& buffer, UserlandHleCall& call);

    std::map<std::uint32_t, Buffer> buffers_;
    std::map<std::uint32_t, std::uint32_t> clients_by_id_;
    std::size_t unsupported_trace_count_{};
    std::optional<std::uint64_t> last_scanout_generation_;
    std::vector<std::uint32_t> last_scanout_pixels_;
    std::shared_ptr<DisplayState> display_;
    std::shared_ptr<SurfaceStore> surfaces_;
    std::shared_ptr<PresentationTracker> presentation_tracker_;
    std::shared_ptr<KernelSharedState> shared_state_;
    std::shared_ptr<SceneCoordinator> scene_coordinator_;
};

} // namespace ilemu
