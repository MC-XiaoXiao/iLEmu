/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "foundation/host_resource_controller.hpp"
#include "foundation/process_loader.hpp"

#include <utility>

namespace ilemu {

class ProcessImagePreparation {
public:
    [[nodiscard]] bool ready() const noexcept
    {
        return ready_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint32_t error() const noexcept { return error_; }
    [[nodiscard]] PreparedProcessImage take_image()
    {
        image_->preparation_admission = std::move(admission_);
        return std::move(*image_);
    }

private:
    friend class HostProcessImagePreparer;
    std::shared_ptr<void> admission_;
    std::optional<PreparedProcessImage> image_;
    std::uint32_t error_ { };
    std::atomic<bool> ready_ { };
};

// Only host path lookup, immutable reads and parsing run here. The kernel
// retains guest inputs and commits process/VM/scheduler effects after ready().
class HostProcessImagePreparer {
public:
    HostProcessImagePreparer(std::filesystem::path rootfs,
        ArmArchitectureVersion architecture);
    ~HostProcessImagePreparer();
    [[nodiscard]] std::shared_ptr<ProcessImagePreparation> prepare(
        std::string path, std::filesystem::path working_directory,
        std::vector<std::string> arguments);

private:
    struct Admission;
    std::filesystem::path rootfs_;
    ArmArchitectureVersion architecture_;
    std::shared_ptr<std::atomic<std::size_t>> admitted_ {
        std::make_shared<std::atomic<std::size_t>>() };
    HostResourceController worker_;
};
} // namespace ilemu
