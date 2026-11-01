// This Source Code Form is subject to the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/file_io.hpp"
#include <algorithm>

namespace ilemu {
bool FileIoOperation::ready() const
{
    return cancelled || error ||
           (sync_request ? sync_request->result().has_value()
                         : request && request->ready());
}
bool FileIoQueue::admits(std::size_t size) const
{
    return operations_.size() < maximum_operations &&
           size <= maximum_bytes - bytes_;
}
void FileIoQueue::push(std::shared_ptr<FileIoOperation> operation)
{
    bytes_ += operation->size;
    operations_.push_back(std::move(operation));
    if (operations_.size() == 1)
        start();
}
std::shared_ptr<FileIoOperation> FileIoQueue::front() const
{
    return operations_.empty() ? nullptr : operations_.front();
}
std::shared_ptr<FileIoOperation> FileIoQueue::admission() const
{
    return admissions_.empty() ? nullptr : admissions_.front();
}
void FileIoQueue::pop()
{
    bytes_ -= operations_.front()->size;
    operations_.pop_front();
    if (!operations_.empty())
        start();
}
void FileIoQueue::start()
{
    auto& operation = *operations_.front();
    if (operation.cancelled || operation.error)
        return;
    if (operation.kind == FileIoOperation::Kind::Synchronize) {
        if (!synchronizer_)
            synchronizer_ = std::make_unique<HostFileSynchronizer>();
        operation.sync_request = synchronizer_->synchronize(
            operation.description->host_descriptor());
    } else {
        if (operation.mapping_identity)
            operation.writeback_revision =
                operation.mapping_identity->writeback_revision.load(
                    std::memory_order_acquire);
        if (!io_)
            io_ = std::make_unique<HostFileIo>();
        const auto position = operation.position.value_or(operation.cursor);
        operation.request = io_->submit(
            operation.description->host_descriptor(), position,
            std::move(operation.input),
            operation.kind == FileIoOperation::Kind::Write, operation.append);
    }
}
void FileIoQueue::repeat_write(FileIoOperation& operation)
{
    const auto offset = operation.request->offset();
    auto bytes = operation.request->take_bytes();
    operation.writeback_revision =
        operation.mapping_identity->writeback_revision.load(
            std::memory_order_acquire);
    operation.request = io_->submit(operation.description->host_descriptor(),
        offset, std::move(bytes), true, false);
}
void FileIoQueue::cancel(const std::shared_ptr<FileIoOperation>& operation)
{
    const auto found =
        std::find(operations_.begin(), operations_.end(), operation);
    const auto first = found == operations_.begin();
    bytes_ -= operation->size;
    operations_.erase(found);
    operation->error = darwin::error::operation_canceled;
    operation->committed = true;
    operation->description.reset();
    operation->mapping_identity.reset();
    operation->input = std::vector<std::byte> { };
    if (first && !operations_.empty())
        start();
}
void FileIoQueue::abandon(std::uint32_t owner)
{
    for (const auto* list : { &operations_, &admissions_ })
        for (const auto& operation : *list)
            if (operation->owner == owner) {
                operation->abandoned = true;
                if (!operation->request && !operation->sync_request)
                    operation->cancelled = true;
            }
}
bool FileIoQueue::owns(std::uint32_t owner) const
{
    for (const auto* list : { &operations_, &admissions_ })
        if (std::any_of(
                list->begin(), list->end(), [owner](const auto& operation) {
                    return operation->owner == owner;
                }))
            return true;
    return false;
}
} // namespace ilemu
