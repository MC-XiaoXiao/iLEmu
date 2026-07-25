#include "ilemu/output.hpp"

#include <stdexcept>

namespace ilemu {

namespace {

bool concise_prefix(std::string_view text) {
    constexpr std::string_view prefixes[]{
        "[control]", "[device-state]", "[loader]", "[clock]",
        "[process]", "[display]", "[baseband]", "[watch]", "[gles]",
        "[perf]", "[boot]", "[benchmark]",
    };
    for (const auto prefix : prefixes) {
        if (text.starts_with(prefix)) return true;
    }
    return false;
}

}  // namespace

Output::Output(std::ostream& stream) : stream_{&stream} {}

Output::Output(const std::filesystem::path& path)
    : file_{std::make_unique<std::ofstream>(path, std::ios::binary | std::ios::trunc)},
      stream_{file_.get()}, flush_each_write_{false} {
    if (!*file_) {
        throw std::runtime_error{"cannot open output file: " + path.string()};
    }
}

void Output::write(std::string_view text) {
    if (!should_emit(text)) return;
    std::lock_guard lock{mutex_};
    stream_->write(text.data(), static_cast<std::streamsize>(text.size()));
    if (flush_each_write_) stream_->flush();
}

void Output::line(std::string_view text) {
    if (!should_emit(text)) return;
    std::lock_guard lock{mutex_};
    stream_->write(text.data(), static_cast<std::streamsize>(text.size()));
    stream_->put('\n');
    if (flush_each_write_) stream_->flush();
}

bool Output::should_emit(std::string_view text) const {
    return verbose_ || text.empty() || text.front() != '[' ||
           concise_prefix(text);
}

}  // namespace ilemu
