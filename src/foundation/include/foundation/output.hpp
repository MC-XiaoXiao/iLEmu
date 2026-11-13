// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Route emulator lifecycle messages, traces and command output to
// configured sinks.

#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <ostream>
#include <string_view>

namespace ilemu {

class Output {
public:
    explicit Output(std::ostream& stream);
    explicit Output(const std::filesystem::path& path);
    ~Output();

    // Interactive frontends default to concise lifecycle output so verbose
    // guest tracing cannot become part of the emulation hot path. Diagnostic
    // commands can retain the constructor's verbose default.
    void set_verbose(bool verbose) { verbose_ = verbose; }
    // Query the same filter before formatting a costly diagnostic. A prefix
    // is sufficient when the complete message has the same output category.
    [[nodiscard]] bool enabled(std::string_view text) const;
    void write(std::string_view text);
    void line(std::string_view text);
    // Emit one explicitly requested low-volume control/attribution marker.
    // File markers carry a flush in the ordered sink queue. Observing one is
    // a causality boundary; a slow sink does not hold the Guest executor.
    void marker(std::string_view text);

private:
    struct FileSink;
    std::unique_ptr<FileSink> file_;
    std::ostream* stream_ { };
    bool flush_each_write_ { true };
    bool verbose_ { true };
    std::mutex mutex_;
};

} // namespace ilemu
