#pragma once

#include <filesystem>
#include <mutex>

namespace ilemu {

struct DisplayFrame;

// Headless display sink that atomically replaces a PNG, BMP, or portable
// pixmap with the most recently presented frame. The suffix selects PNG/BMP;
// other suffixes retain the original PPM output for compatibility.
class FrameFilePresenter {
public:
    explicit FrameFilePresenter(std::filesystem::path path);

    void present(const DisplayFrame& frame);

private:
    std::filesystem::path path_;
    std::mutex mutex_;
};

}  // namespace ilemu
