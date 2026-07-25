#include "ilemu/lockdown_profile.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace ilemu {
namespace {

constexpr std::string_view activation_state_key{"-ActivationState"};
constexpr std::string_view activation_acknowledged_key{
    "-ActivationStateAcknowledged"};

std::string initial_plist() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple Computer//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n</dict>\n</plist>\n";
}

std::string read_file(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return initial_plist();
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"could not read Lockdown state: " +
                                 path.string()};
    }
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

std::size_t value_end(const std::string& xml, std::size_t begin) {
    if (xml.compare(begin, 7, "<true/>") == 0) return begin + 7;
    if (xml.compare(begin, 8, "<false/>") == 0) return begin + 8;
    const auto tag_end = xml.find('>', begin);
    if (tag_end == std::string::npos || xml[begin] != '<') {
        throw std::runtime_error{"malformed Lockdown data_ark value"};
    }
    const auto name_end = xml.find_first_of(" >", begin + 1);
    if (name_end == std::string::npos || name_end > tag_end) {
        throw std::runtime_error{"malformed Lockdown data_ark tag"};
    }
    const auto closing = "</" + xml.substr(begin + 1, name_end - begin - 1) +
                         ">";
    const auto end = xml.find(closing, tag_end + 1);
    if (end == std::string::npos) {
        throw std::runtime_error{"unterminated Lockdown data_ark value"};
    }
    return end + closing.size();
}

void upsert(std::string& xml, std::string_view key,
            std::string_view encoded_value) {
    const auto encoded_key = "<key>" + std::string{key} + "</key>";
    const auto key_position = xml.find(encoded_key);
    if (key_position == std::string::npos) {
        const auto dictionary_end = xml.rfind("</dict>");
        if (dictionary_end == std::string::npos) {
            throw std::runtime_error{"Lockdown data_ark has no dictionary"};
        }
        const auto entry = "\t" + encoded_key + "\n\t" +
                           std::string{encoded_value} + "\n";
        xml.insert(dictionary_end, entry);
        return;
    }
    const auto value_begin = xml.find('<', key_position + encoded_key.size());
    if (value_begin == std::string::npos) {
        throw std::runtime_error{"Lockdown data_ark key has no value"};
    }
    xml.replace(value_begin, value_end(xml, value_begin) - value_begin,
                encoded_value);
}

}  // namespace

std::optional<LockdownActivation> parse_lockdown_activation(
    std::string_view value) {
    if (value == "preserve") return LockdownActivation::Preserve;
    if (value == "activated") return LockdownActivation::Activated;
    if (value == "unactivated") return LockdownActivation::Unactivated;
    return std::nullopt;
}

LockdownProfileResult apply_lockdown_profile(
    const std::filesystem::path& rootfs, LockdownActivation activation) {
    const auto path = rootfs / "private/var/root/Library/Lockdown/data_ark.plist";
    if (activation == LockdownActivation::Preserve) return {path, false};

    auto xml = read_file(path);
    const auto original = xml;
    const auto activated = activation == LockdownActivation::Activated;
    upsert(xml, activation_state_key,
           activated ? "<string>Activated</string>"
                     : "<string>Unactivated</string>");
    upsert(xml, activation_acknowledged_key,
           activated ? "<true/>" : "<false/>");
    if (xml == original) return {path, false};

    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".ilemu.tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output || !output.write(xml.data(),
                                     static_cast<std::streamsize>(xml.size()))) {
            throw std::runtime_error{"could not write Lockdown state: " +
                                     temporary.string()};
        }
    }
    std::filesystem::rename(temporary, path);
    return {path, true};
}

}  // namespace ilemu
