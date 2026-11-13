// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Route emulator lifecycle messages, traces and command output to
// configured sinks.

#include "foundation/output.hpp"
#include "foundation/host_resource_controller.hpp"

#include <condition_variable>
#include <deque>
#include <stdexcept>
#include <string>

namespace ilemu {

namespace {

    bool concise_prefix(std::string_view text)
    {
        constexpr std::string_view prefixes[] {
            "[control]",
            "[device-state]",
            "[loader]",
            "[clock]",
            "[process]",
            "[cpu] fatal",
            "[cpu] exception",
            "[display]",
            "[baseband]",
            "[watch]",
            "[gles]",
            "[perf]",
            "[perf-display]",
            "[perf-clock]",
            "[perf-artifact]",
            "[perf-artifact-validation]",
            "[perf-reclamation]",
            "[perf-jit-profile]",
            "[jit-tiered]",
            "[precompile]",
            "[precompile-outcomes]",
            "[precompile-schedule]",
            "[boot]",
            "[benchmark]",
            "[transition]",
            "[transition-snapshot]",
            "[display-attribution]",
        };
        for (const auto prefix : prefixes) {
            if (text.starts_with(prefix))
                return true;
        }
        return false;
    }

} // namespace

// Keep disk writes outside the executor and outside the producer mutex. The
// existing mandatory host-work lane preserves FIFO and drains before close.
struct Output::FileSink {
    struct Record {
        std::string text;
        bool flush;
    };

    explicit FileSink(const std::filesystem::path& path)
        : stream { path, std::ios::binary | std::ios::trunc }
    {
        if (!stream)
            throw std::runtime_error { "cannot open output file: " + path.string() };
    }

    ~FileSink()
    {
        worker.wait_idle();
        stream.flush();
    }

    void enqueue(std::string_view text, bool newline, bool flush)
    {
        const auto size = text.size() + static_cast<std::size_t>(newline);
        std::unique_lock lock { mutex };
        // Backpressure bounds retained log data without dropping records. One
        // oversized diagnostic record may occupy the empty queue on its own.
        space.wait(lock, [&] {
            return pending_bytes == 0 ||
                   (pending_bytes <= maximum_pending_bytes &&
                       size <= maximum_pending_bytes - pending_bytes);
        });
        auto& record = records.emplace_back(std::string { text }, flush);
        if (newline)
            record.text.push_back('\n');
        pending_bytes += size;
        if (!scheduled) {
            scheduled = true;
            static_cast<void>(worker.submit(HostWorkKind::Maintenance,
                std::nullopt, [this] { drain(); }));
        }
    }

    void drain()
    {
        for (;;) {
            Record record;
            {
                std::lock_guard lock { mutex };
                if (records.empty()) {
                    scheduled = false;
                    return;
                }
                record = std::move(records.front());
                records.pop_front();
            }
            stream.write(record.text.data(),
                static_cast<std::streamsize>(record.text.size()));
            if (record.flush)
                stream.flush();
            {
                std::lock_guard lock { mutex };
                pending_bytes -= record.text.size();
            }
            space.notify_all();
        }
    }

    static constexpr std::size_t maximum_pending_bytes = 4U * 1024U * 1024U;
    std::ofstream stream;
    std::mutex mutex;
    std::condition_variable space;
    std::deque<Record> records;
    std::size_t pending_bytes { };
    bool scheduled { };
    HostResourceController worker;
};

Output::Output(std::ostream& stream)
    : stream_ { &stream }
{
}

Output::Output(const std::filesystem::path& path)
    : file_ { std::make_unique<FileSink>(path) }
    , flush_each_write_ { false }
{
}

Output::~Output() = default;

void Output::write(std::string_view text)
{
    if (!enabled(text))
        return;
    if (file_) {
        file_->enqueue(text, false, false);
        return;
    }
    std::lock_guard lock { mutex_ };
    stream_->write(text.data(), static_cast<std::streamsize>(text.size()));
    if (flush_each_write_)
        stream_->flush();
}

void Output::line(std::string_view text)
{
    if (!enabled(text))
        return;
    if (file_) {
        file_->enqueue(text, true, text.starts_with("[perf-display]"));
        return;
    }
    std::lock_guard lock { mutex_ };
    stream_->write(text.data(), static_cast<std::streamsize>(text.size()));
    stream_->put('\n');
    // Interactive performance windows end outside the measured interval, so
    // flushing their one-line result cannot perturb the sample and lets a
    // controller evaluate it before starting the next window.
    if (flush_each_write_ || text.starts_with("[perf-display]"))
        stream_->flush();
}

void Output::marker(std::string_view text)
{
    if (!enabled(text))
        return;
    if (file_) {
        file_->enqueue(text, true, true);
        return;
    }
    std::lock_guard lock { mutex_ };
    stream_->write(text.data(), static_cast<std::streamsize>(text.size()));
    stream_->put('\n');
    stream_->flush();
}

bool Output::enabled(std::string_view text) const
{
    return verbose_ || text.empty() || text.front() != '[' ||
           concise_prefix(text);
}

} // namespace ilemu
