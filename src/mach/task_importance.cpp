/* SPDX-License-Identifier: MPL-2.0 */
// Assertion transitions follow XNU3789 ipc_importance.c/task_policy.c.
// https://github.com/apple-oss-distributions/xnu/blob/xnu-3789.1.32/osfmk/ipc/ipc_importance.c
#include "mach/task_importance.hpp"

#include <cassert>
#include <utility>

namespace ilemu::xnu::ipc {

std::recursive_mutex ImportanceElement::mutex_;

std::shared_ptr<ImportanceValue> ImportanceValue::create(
    std::shared_ptr<ImportanceElement> element)
{
    std::lock_guard lock { ImportanceElement::mutex_ };
    auto value = element->voucher_value_.lock();
    if (!value) {
        value = std::shared_ptr<ImportanceValue> {
            new ImportanceValue { element } };
        element->voucher_value_ = value;
    }
    return value;
}

ImportanceValue::ImportanceValue(std::shared_ptr<ImportanceElement> element)
    : element_ { std::move(element) }
{
    ++element_->value_owners_;
}

ImportanceValue::~ImportanceValue()
{
    std::lock_guard lock { ImportanceElement::mutex_ };
    assert(element_->value_owners_ != 0);
    if (--element_->value_owners_ == 0) {
        const auto inherit = std::dynamic_pointer_cast<ImportanceAssertion>(element_);
        if (inherit && inherit->inheritance_)
            inherit->release_value();
    }
}

void ImportanceElement::publish(bool donating)
{
    if (donating_.exchange(donating, std::memory_order_relaxed) == donating)
        return;
    // Propagation only changes assertion state; owners retain every link
    // throughout this transaction, as with XNU's locked propagation lists.
    for (auto* link : downstream_)
        link->source_changed();
}

void TaskImportance::set_traits(Traits traits, bool live_donor_active)
{
    std::lock_guard lock { mutex_ };
    traits_ = traits;
    if (traits_.live_donor)
        traits_.donor = live_donor_active;
    recompute();
}

void TaskImportance::set_donor(bool donor)
{
    std::lock_guard lock { mutex_ };
    traits_.donor = donor;
    recompute();
}

void TaskImportance::set_live_donor_active(bool active)
{
    std::lock_guard lock { mutex_ };
    // XNU's live-donor update replaces iit_donor; it is not an additional
    // permanent donor reason beside an earlier explicit mark.
    if (!traits_.live_donor)
        return;
    traits_.donor = active;
    recompute();
}

void TaskImportance::retire()
{
    std::lock_guard lock { mutex_ };
    retired_ = true;
    assertions_ -= legacy_assertions_;
    legacy_assertions_ = 0;
    recompute();
}

void TaskImportance::reset_legacy()
{
    std::lock_guard lock { mutex_ };
    assertions_ -= legacy_assertions_;
    legacy_assertions_ = 0;
    recompute();
}

bool TaskImportance::receiver() const
{
    std::lock_guard lock { mutex_ };
    return !retired_ && traits_.receiver;
}

bool TaskImportance::accepts() const
{
    std::lock_guard lock { mutex_ };
    return !retired_ && (traits_.receiver || traits_.denap_receiver);
}

bool TaskImportance::never_donor() const
{
    std::lock_guard lock { mutex_ };
    return retired_ || (!traits_.donor && !traits_.live_donor && !traits_.receiver);
}

std::uint32_t TaskImportance::legacy_assertions() const
{
    std::lock_guard lock { mutex_ };
    return legacy_assertions_;
}

void TaskImportance::adjust_assertions(std::int64_t delta)
{
    assert(delta >= 0 || assertions_ >= static_cast<std::uint64_t>(-delta));
    assertions_ = delta >= 0 ? assertions_ + static_cast<std::uint64_t>(delta)
                            : assertions_ - static_cast<std::uint64_t>(-delta);
    recompute();
}

void TaskImportance::recompute()
{
    const bool boosted = !retired_ && assertions_ != 0 &&
                         (traits_.receiver || traits_.denap_receiver);
    boosted_.store(boosted, std::memory_order_relaxed);
    publish(!retired_ && (traits_.donor || (traits_.receiver && boosted)));
}

bool TaskImportance::hold_legacy()
{
    std::lock_guard lock { mutex_ };
    if (retired_ || legacy_assertions_ == 0)
        return false;
    ++legacy_assertions_;
    adjust_assertions(1);
    return true;
}

bool TaskImportance::drop_legacy()
{
    std::lock_guard lock { mutex_ };
    if (legacy_assertions_ == 0)
        return false;
    --legacy_assertions_;
    adjust_assertions(-1);
    return true;
}

std::shared_ptr<ImportanceAssertion> ImportanceAssertion::create(
    std::shared_ptr<ImportanceRecipient> target,
    std::shared_ptr<ImportanceElement> source,
    std::optional<bool> initially_donating)
{
    std::lock_guard lock { mutex_ };
    if (!target->accepts())
        return { };
    return std::shared_ptr<ImportanceAssertion> {
        new ImportanceAssertion { std::move(target), std::move(source), initially_donating } };
}

ImportanceAssertion::ImportanceAssertion(
    std::shared_ptr<ImportanceRecipient> target,
    std::shared_ptr<ImportanceElement> source,
    std::optional<bool> initially_donating)
    : target_ { std::move(target) }, source_ { std::move(source) }
{
    propagation_source_ = source_;
    if (const auto parent = std::dynamic_pointer_cast<ImportanceAssertion>(source_)) {
        if (const auto sender = std::dynamic_pointer_cast<TaskImportance>(parent->target_))
            propagation_source_ = sender;
    }
    // Native kmsg/inherit links are indexed on the sender task even when the
    // retained provenance element is another inheritance value.
    if (propagation_source_) {
        source_link_ = propagation_source_->downstream_.insert(
            propagation_source_->downstream_.end(), this);
        linked_ = true;
    }
    set_donating(initially_donating.value_or(
        !propagation_source_ || propagation_source_->donating()));
}

ImportanceAssertion::~ImportanceAssertion()
{
    std::lock_guard lock { mutex_ };
    if (inheritance_) {
        const auto task = std::static_pointer_cast<TaskImportance>(target_);
        const auto found = task->inheritances_.find(inheritance_key_);
        if (found != task->inheritances_.end()) {
            const auto current = found->second.lock();
            if (!current || current.get() == this)
                task->inheritances_.erase(found);
        }
    }
    cancel();
}

void ImportanceAssertion::disconnect()
{
    if (linked_) {
        propagation_source_->downstream_.erase(source_link_);
        linked_ = false;
    }
    propagation_source_.reset();
    source_.reset();
}

void ImportanceAssertion::set_donating(bool donating)
{
    if (this->donating() == donating)
        return;
    // Mark before changing the task's assertion count. Reentrant propagation
    // through another message then observes this link's transition barrier.
    donating_.store(donating, std::memory_order_relaxed);
    const auto count = static_cast<std::int64_t>(inheritance_ ? external_refs_ : 1U);
    target_->adjust_assertions(donating ? count : -count);
    for (auto* link : downstream_)
        link->source_changed();
}

void ImportanceAssertion::source_changed()
{
    set_donating(active_ && (!inheritance_ || external_refs_ != 0) &&
                 (!propagation_source_ || propagation_source_->donating()));
}

void ImportanceAssertion::cancel()
{
    std::lock_guard lock { mutex_ };
    active_ = false;
    disconnect();
    set_donating(false);
}

std::shared_ptr<ImportanceRecipient> ImportanceAssertion::target() const
{
    std::lock_guard lock { mutex_ };
    return target_;
}

void ImportanceAssertion::retarget(std::shared_ptr<ImportanceRecipient> target)
{
    std::lock_guard lock { mutex_ };
    assert(!inheritance_);
    if (target_ == target)
        return;
    const auto held = donating();
    // Transfer first, then release the old target, under one importance lock.
    auto previous = std::exchange(target_, std::move(target));
    if (held) {
        target_->adjust_assertions(1);
        previous->adjust_assertions(-1);
    }
}

bool ImportanceAssertion::externalize_legacy()
{
    std::lock_guard lock { mutex_ };
    const auto task = std::dynamic_pointer_cast<TaskImportance>(target_);
    if (!active_ || !donating() || !task || !task->accepts())
        return false;
    ++task->legacy_assertions_;
    task->adjust_assertions(1);
    cancel();
    return true;
}

std::shared_ptr<ImportanceAssertion> ImportanceAssertion::inherit(
    std::shared_ptr<TaskImportance> target,
    std::shared_ptr<ImportanceElement> source, bool donating)
{
    std::lock_guard lock { mutex_ };
    if (!target->accepts())
        return { };
    std::uint32_t depth = 1;
    std::shared_ptr<ImportanceElement> sender = source;
    if (const auto parent = std::dynamic_pointer_cast<ImportanceAssertion>(source);
        parent && parent->inheritance_) {
        if (parent->target_ == target) {
            // Do not turn a non-donating context into a self donation using
            // some other boost currently held by this task.
            parent->adjust_external(1);
            return parent;
        }
        sender = std::static_pointer_cast<TaskImportance>(parent->target_);
        if (parent->depth_ == 32U)
            source = sender;
        else
            depth = parent->depth_ + 1U;
    }
    if (source.get() == target.get())
        return { };
    const auto key = std::pair { sender ? sender.get() : target.get(), depth };
    auto& cached = target->inheritances_[key];
    auto value = cached.lock();
    if (value && value->active_) {
        const auto was_empty = value->external_refs_ == 0;
        value->adjust_external(1);
        if (was_empty)
            value->set_donating(donating);
        return value;
    }
    value = create(target, source, donating);
    value->inheritance_ = true;
    value->external_refs_ = 1;
    value->depth_ = depth;
    value->inheritance_key_ = key;
    cached = value;
    return value;
}

std::shared_ptr<ImportanceElement> ImportanceAssertion::source() const
{
    std::lock_guard lock { mutex_ };
    return source_;
}

std::uint32_t ImportanceAssertion::external_refs() const
{
    std::lock_guard lock { mutex_ };
    return external_refs_;
}

std::uint32_t ImportanceAssertion::drop_external(std::uint32_t count)
{
    std::lock_guard lock { mutex_ };
    if (!inheritance_)
        return 4U; // KERN_INVALID_ARGUMENT
    const auto task = std::static_pointer_cast<TaskImportance>(target_);
    if (!task->traits_.denap_receiver)
        return 16U; // KERN_INVALID_TASK
    if (count > external_refs_)
        return 5U; // KERN_FAILURE
    adjust_external(-static_cast<std::int64_t>(count));
    return 0U;
}

void ImportanceAssertion::undo_receive_external()
{
    std::lock_guard lock { mutex_ };
    assert(inheritance_ && external_refs_ != 0);
    adjust_external(-1);
}

void ImportanceAssertion::adjust_external(std::int64_t delta)
{
    assert(delta >= 0 || external_refs_ >= static_cast<std::uint64_t>(-delta));
    external_refs_ = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(external_refs_) + delta);
    const auto held = donating();
    if (external_refs_ == 0)
        donating_.store(false, std::memory_order_relaxed);
    if (held)
        target_->adjust_assertions(delta);
}

void ImportanceAssertion::release_value()
{
    std::lock_guard lock { mutex_ };
    adjust_external(-static_cast<std::int64_t>(external_refs_));
}

void ImportancePort::update_route()
{
    std::shared_ptr<ImportanceRecipient> next;
    if (active_) {
        if (transit_)
            next = transit_;
        else {
            const auto& task = temporary_owner_ ? watch_ : owner_;
            if (task && task->accepts())
                next = task;
        }
    }
    if (next == route_)
        return;
    const auto count = static_cast<std::int64_t>(assertions_);
    auto previous = std::exchange(route_, std::move(next));
    if (route_ && count != 0)
        route_->adjust_assertions(count);
    if (previous && count != 0)
        previous->adjust_assertions(-count);
}

void ImportancePort::adjust_assertions(std::int64_t delta)
{
    assert(delta >= 0 || assertions_ >= static_cast<std::uint64_t>(-delta));
    assertions_ = delta >= 0 ? assertions_ + static_cast<std::uint64_t>(delta)
                            : assertions_ - static_cast<std::uint64_t>(-delta);
    if (route_)
        route_->adjust_assertions(delta);
}

void ImportancePort::set_owner(std::shared_ptr<TaskImportance> owner)
{
    std::lock_guard lock { ImportanceElement::mutex_ };
    owner_ = std::move(owner);
    watch_.reset();
    transit_.reset();
    temporary_owner_ = false;
    update_route();
}

void ImportancePort::set_temporary_owner()
{
    std::lock_guard lock { ImportanceElement::mutex_ };
    temporary_owner_ = true;
    watch_.reset();
    update_route();
}

bool ImportancePort::set_watch_task(std::shared_ptr<TaskImportance> task)
{
    std::lock_guard lock { ImportanceElement::mutex_ };
    if (!temporary_owner_ || transit_ || !task->accepts())
        return false;
    watch_ = std::move(task);
    update_route();
    return true;
}

void ImportancePort::set_transit(std::shared_ptr<ImportancePort> destination)
{
    std::lock_guard lock { ImportanceElement::mutex_ };
    owner_.reset();
    watch_.reset();
    temporary_owner_ = false;
    transit_ = std::move(destination);
    update_route();
}

void ImportancePort::clear_receiver()
{
    std::lock_guard lock { ImportanceElement::mutex_ };
    owner_.reset();
    // ip_imp_task retains a temporary owner's watch binding until circularity
    // links the receive right to its destination (or the right is destroyed).
    if (!temporary_owner_)
        watch_.reset();
    transit_.reset();
    update_route();
}

void ImportancePort::deactivate()
{
    std::lock_guard lock { ImportanceElement::mutex_ };
    active_ = false;
    temporary_owner_ = false;
    clear_receiver();
}

bool ImportancePort::accepts() const
{
    std::lock_guard lock { ImportanceElement::mutex_ };
    return active_;
}

std::uint64_t ImportancePort::assertions() const
{
    std::lock_guard lock { ImportanceElement::mutex_ };
    return assertions_;
}

} // namespace ilemu::xnu::ipc
