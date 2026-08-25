#include "ilemu/live_button_scheduler.hpp"

#include <algorithm>

namespace ilemu {

void LiveButtonScheduler::schedule(
    SystemButtonInput down, std::chrono::milliseconds hold)
{
    if (down.phase != SystemButtonPhase::Down ||
        hold <= std::chrono::milliseconds::zero()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto start = events_.empty()
                           ? now
                           : std::max(now, events_.back().deadline +
                                               std::chrono::milliseconds { 1 });
    events_.push_back(Event { start + hold,
        SystemButtonInput { down.button, SystemButtonPhase::Up } });
}

std::vector<SystemButtonInput> LiveButtonScheduler::poll()
{
    std::vector<SystemButtonInput> result;
    const auto now = std::chrono::steady_clock::now();
    while (!events_.empty() && events_.front().deadline <= now) {
        result.push_back(events_.front().input);
        events_.pop_front();
    }
    return result;
}

std::optional<std::chrono::steady_clock::time_point>
LiveButtonScheduler::next_deadline() const
{
    return events_.empty()
               ? std::nullopt
               : std::optional<std::chrono::steady_clock::time_point> {
                     events_.front().deadline
                 };
}

} // namespace ilemu
