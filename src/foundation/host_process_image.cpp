/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/host_process_image.hpp"
#include "foundation/rootfs_path_resolver.hpp"

namespace ilemu {
namespace {
    constexpr std::size_t maximum_pending_images = 64;
}
struct HostProcessImagePreparer::Admission {
    explicit Admission(std::shared_ptr<std::atomic<std::size_t>> count)
        : count { std::move(count) }
    {
        this->count->fetch_add(1, std::memory_order_relaxed);
    }
    ~Admission() { count->fetch_sub(1, std::memory_order_release); }
    std::shared_ptr<std::atomic<std::size_t>> count;
};

HostProcessImagePreparer::HostProcessImagePreparer(std::filesystem::path rootfs,
    ArmArchitectureVersion architecture)
    : rootfs_ { std::move(rootfs) }, architecture_ { architecture }
{
}
HostProcessImagePreparer::~HostProcessImagePreparer() { worker_.wait_idle(); }

std::shared_ptr<ProcessImagePreparation> HostProcessImagePreparer::prepare(
    std::string path, std::filesystem::path working_directory,
    std::vector<std::string> arguments)
{
    // One serial kernel producer. The lease covers queued, active and completed
    // results, including cancelled queue entries until the worker visits them.
    if (admitted_->load(std::memory_order_acquire) >= maximum_pending_images)
        return { };
    auto admission = std::make_shared<Admission>(admitted_);
    auto request = std::make_shared<ProcessImagePreparation>();
    request->admission_ = admission;
    const auto task = worker_.submit(HostWorkKind::Maintenance, std::nullopt,
        [pending = std::weak_ptr { request }, admission = std::move(admission),
            rootfs = rootfs_, architecture = architecture_,
            path = std::move(path), cwd = std::move(working_directory),
            arguments = std::move(arguments)]() mutable {
            auto request = pending.lock();
            if (!request)
                return;
            try {
                const auto host = RootfsPathResolver { rootfs }.resolve(path, cwd);
                std::error_code error;
                if (!std::filesystem::is_regular_file(host, error)) {
                    request->error_ = 2; // ENOENT, matching exec/PATH lookup.
                } else {
                    if (std::filesystem::path { path }.is_relative())
                        path = (std::filesystem::path { "/" } /
                            host.lexically_relative(rootfs)).generic_string();
                    request->image_ = ProcessImageLoader { rootfs, architecture }
                        .prepare(std::move(path), std::move(arguments));
                }
            } catch (const std::exception&) {
                request->error_ = 8; // ENOEXEC
            }
            request->ready_.store(true, std::memory_order_release);
        });
    return task ? request : nullptr;
}
} // namespace ilemu
