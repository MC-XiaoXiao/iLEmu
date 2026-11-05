/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <utility>

namespace ilemu::xnu::ipc {

class ImportanceAssertion;
class ImportancePort;
class ImportanceValue;

// XNU3789 ipc_importance.c: task/inherit elements own downstream propagation
// links; messages and vouchers own the links themselves. A single importance
// lock serializes the assertion transfer and propagation transaction.
class ImportanceElement {
public:
    virtual ~ImportanceElement() = default;
    [[nodiscard]] bool donating() const noexcept
    {
        return donating_.load(std::memory_order_relaxed);
    }

protected:
    friend class ImportanceAssertion;
    friend class ImportancePort;
    friend class ImportanceValue;
    static std::recursive_mutex mutex_;
    void publish(bool donating);
    std::atomic_bool donating_ { };
    std::list<ImportanceAssertion*> downstream_;
    std::weak_ptr<ImportanceValue> voucher_value_;
    std::uint32_t value_owners_ { };
};

// Voucher-manager ownership is separate from propagation references. A child
// inheritance may keep an element alive after its last voucher value releases
// all external assertions, just as XNU's iie_refs differs from iie_made.
class ImportanceValue final {
public:
    static std::shared_ptr<ImportanceValue> create(
        std::shared_ptr<ImportanceElement> element);
    ~ImportanceValue();
    ImportanceValue(const ImportanceValue&) = delete;
    ImportanceValue& operator=(const ImportanceValue&) = delete;
    [[nodiscard]] const std::shared_ptr<ImportanceElement>& element() const
    {
        return element_;
    }
private:
    explicit ImportanceValue(std::shared_ptr<ImportanceElement> element);
    std::shared_ptr<ImportanceElement> element_;
};

class ImportanceRecipient {
public:
    virtual ~ImportanceRecipient() = default;
    [[nodiscard]] virtual bool accepts() const = 0;
private:
    friend class ImportanceAssertion;
    friend class ImportancePort;
    virtual void adjust_assertions(std::int64_t delta) = 0;
};

class TaskImportance final : public ImportanceElement, public ImportanceRecipient {
public:
    struct Traits {
        bool donor { };
        bool live_donor { };
        bool receiver { };
        bool denap_receiver { };
    };

    void set_traits(Traits traits, bool live_donor_active = false);
    void set_donor(bool donor);
    void set_live_donor_active(bool active);
    void retire();
    void reset_legacy();
    [[nodiscard]] bool receiver() const;
    [[nodiscard]] bool accepts() const override;
    [[nodiscard]] bool never_donor() const;
    [[nodiscard]] bool boosted() const noexcept
    {
        return boosted_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t legacy_assertions() const;
    [[nodiscard]] bool hold_legacy();
    [[nodiscard]] bool drop_legacy();

private:
    friend class ImportanceAssertion;
    friend class ImportancePort;
    void adjust_assertions(std::int64_t delta) override;
    void recompute();
    Traits traits_;
    bool retired_ { };
    std::uint64_t assertions_ { };
    std::uint32_t legacy_assertions_ { };
    std::atomic_bool boosted_ { };
    std::map<std::pair<ImportanceElement*, std::uint32_t>,
        std::weak_ptr<ImportanceAssertion>> inheritances_;
};

// Port importance survives receive-right transit and may accumulate before
// launchd binds its temporary-owner watchport to the provider task.
class ImportancePort final : public ImportanceRecipient {
public:
    void set_owner(std::shared_ptr<TaskImportance> owner);
    void set_temporary_owner();
    bool set_watch_task(std::shared_ptr<TaskImportance> task);
    void set_transit(std::shared_ptr<ImportancePort> destination);
    void clear_receiver();
    void deactivate();
    [[nodiscard]] bool accepts() const override;
    [[nodiscard]] std::uint64_t assertions() const;
private:
    void adjust_assertions(std::int64_t delta) override;
    void update_route();
    std::shared_ptr<TaskImportance> owner_;
    std::shared_ptr<TaskImportance> watch_;
    std::shared_ptr<ImportancePort> transit_;
    std::shared_ptr<ImportanceRecipient> route_;
    std::uint64_t assertions_ { };
    bool temporary_owner_ { };
    bool active_ { true };
};

// Shared owners retain one assertion. Source changes redrive its descendants;
// its last owner drops the target's assertion. Transport adapters distinguish
// queued-port ownership from immutable voucher inheritance at receive.
class ImportanceAssertion final : public ImportanceElement {
public:
    static std::shared_ptr<ImportanceAssertion> create(
        std::shared_ptr<ImportanceRecipient> target,
        std::shared_ptr<ImportanceElement> source = { },
        std::optional<bool> initially_donating = std::nullopt);
    static std::shared_ptr<ImportanceAssertion> inherit(
        std::shared_ptr<TaskImportance> target,
        std::shared_ptr<ImportanceElement> source, bool donating);
    ~ImportanceAssertion() override;
    void cancel();
    void retarget(std::shared_ptr<ImportanceRecipient> target);
    [[nodiscard]] bool externalize_legacy();
    [[nodiscard]] std::shared_ptr<ImportanceElement> source() const;
    [[nodiscard]] bool inheritance() const noexcept { return inheritance_; }
    [[nodiscard]] std::uint32_t external_refs() const;
    std::uint32_t drop_external(std::uint32_t count);
    void undo_receive_external();
    [[nodiscard]] std::shared_ptr<ImportanceRecipient> target() const;

private:
    friend class ImportanceElement;
    friend class ImportanceValue;
    ImportanceAssertion(std::shared_ptr<ImportanceRecipient> target,
        std::shared_ptr<ImportanceElement> source,
        std::optional<bool> initially_donating);
    void source_changed();
    void set_donating(bool donating);
    void disconnect();
    void release_value();
    void adjust_external(std::int64_t delta);
    std::shared_ptr<ImportanceRecipient> target_;
    std::shared_ptr<ImportanceElement> source_;
    std::shared_ptr<ImportanceElement> propagation_source_;
    std::list<ImportanceAssertion*>::iterator source_link_;
    bool linked_ { };
    bool active_ { true };
    bool inheritance_ { };
    std::uint32_t external_refs_ { };
    std::uint32_t depth_ { };
    std::pair<ImportanceElement*, std::uint32_t> inheritance_key_ { };
};

} // namespace ilemu::xnu::ipc
