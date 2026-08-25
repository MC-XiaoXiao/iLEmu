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

    // Interactive frontends default to concise lifecycle output so verbose
    // guest tracing cannot become part of the emulation hot path. Diagnostic
    // commands can retain the constructor's verbose default.
    void set_verbose(bool verbose) { verbose_ = verbose; }
    void write(std::string_view text);
    void line(std::string_view text);
    // Emit one explicitly requested low-volume control/attribution marker.
    // Unlike ordinary file output, markers are flushed so an external
    // controller can use them as a causality boundary without flushing the
    // emulation log or every frame.
    void marker(std::string_view text);

private:
    [[nodiscard]] bool should_emit(std::string_view text) const;

    std::unique_ptr<std::ofstream> file_;
    std::ostream* stream_ { };
    bool flush_each_write_ { true };
    bool verbose_ { true };
    std::mutex mutex_;
};

} // namespace ilemu
