#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Arguments {
    std::filesystem::path input;
    std::filesystem::path output;
    std::string cpp_namespace;
    std::vector<std::filesystem::path> include_directories;
    std::vector<std::filesystem::path> type_definitions;
    std::vector<std::pair<std::string, std::string>> type_aliases;
};

enum class WireType {
    Unknown,
    Scalar,
    Port,
    FixedInline,
    VariableInline,
    OutOfLine,
    OutOfLinePorts,
};

struct Routine {
    std::string name;
    std::uint32_t identifier { };
    struct Argument {
        enum class Direction { In, Out, InOut };
        std::string name;
        std::string type;
        std::string attributes;
        Direction direction { Direction::In };
        WireType wire_type { WireType::Unknown };
        std::uint32_t wire_size { };
        std::uint32_t count_prefix_size { };
        std::uint32_t element_size { };
        std::uint32_t request_offset { 0xffff'ffffU };
        std::uint32_t reply_offset { 0xffff'ffffU };
        std::uint32_t request_count_offset { 0xffff'ffffU };
        std::uint32_t reply_count_offset { 0xffff'ffffU };
    };
    std::vector<Argument> arguments;
};

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream stream { path };
    if (!stream)
        throw std::runtime_error { "cannot open " + path.string() };
    return { std::istreambuf_iterator<char> { stream },
        std::istreambuf_iterator<char> { } };
}

std::string remove_comments(std::string_view source)
{
    enum class State { Text, Slash, Block, BlockStar, Line };
    State state = State::Text;
    std::string result;
    result.reserve(source.size());
    for (const char value : source) {
        switch (state) {
        case State::Text:
            if (value == '/') {
                state = State::Slash;
            } else {
                result.push_back(value);
            }
            break;
        case State::Slash:
            if (value == '*') {
                state = State::Block;
            } else if (value == '/') {
                state = State::Line;
            } else {
                result.push_back('/');
                result.push_back(value);
                state = State::Text;
            }
            break;
        case State::Block:
            if (value == '*')
                state = State::BlockStar;
            if (value == '\n')
                result.push_back(value);
            break;
        case State::BlockStar:
            if (value == '/') {
                state = State::Text;
            } else {
                if (value == '\n')
                    result.push_back(value);
                state = value == '*' ? State::BlockStar : State::Block;
            }
            break;
        case State::Line:
            if (value == '\n') {
                result.push_back(value);
                state = State::Text;
            }
            break;
        }
    }
    if (state == State::Slash)
        result.push_back('/');
    return result;
}

std::string_view trim(std::string_view value)
{
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

// The checked-in XNU definitions use a small, regular preprocessor subset.
// Evaluate it as a kernel-server build so disabled routines do not get counted
// together with the `skip` occupying the same MIG ordinal.
std::string preprocess_kernel_server(std::string_view source)
{
    struct Conditional {
        bool parent_active { };
        bool condition { };
        bool saw_else { };
    };
    std::vector<Conditional> conditionals;
    bool active = true;
    std::string result;
    std::istringstream lines { std::string { source } };
    std::string line;
    while (std::getline(lines, line)) {
        const auto directive = trim(line);
        const auto begins = [&](std::string_view prefix) {
            return directive.starts_with(prefix) &&
                   (directive.size() == prefix.size() ||
                       std::isspace(static_cast<unsigned char>(
                           directive[prefix.size()])));
        };
        if (begins("#if") || begins("#ifdef") || begins("#ifndef")) {
            bool condition = true;
            if (begins("#if")) {
                condition = trim(directive.substr(3)) != "0";
            } else if (begins("#ifndef")) {
                condition = false;
            }
            conditionals.push_back({ active, condition, false });
            active = active && condition;
            continue;
        }
        if (begins("#else")) {
            if (conditionals.empty() || conditionals.back().saw_else) {
                throw std::runtime_error { "unmatched or repeated #else" };
            }
            auto& conditional = conditionals.back();
            conditional.saw_else = true;
            active = conditional.parent_active && !conditional.condition;
            continue;
        }
        if (begins("#endif")) {
            if (conditionals.empty()) {
                throw std::runtime_error { "unmatched #endif" };
            }
            active = conditionals.back().parent_active;
            conditionals.pop_back();
            continue;
        }
        if (active) {
            result.append(line);
            result.push_back('\n');
        }
    }
    if (!conditionals.empty()) {
        throw std::runtime_error { "unterminated preprocessor conditional" };
    }
    return result;
}

bool valid_identifier(std::string_view value)
{
    if (value.empty() ||
        !(std::isalpha(static_cast<unsigned char>(value.front())) ||
            value.front() == '_')) {
        return false;
    }
    for (const char character : value.substr(1)) {
        if (!std::isalnum(static_cast<unsigned char>(character)) &&
            character != '_') {
            return false;
        }
    }
    return true;
}

std::string normalize_whitespace(std::string_view value)
{
    std::string result;
    bool pending_space = false;
    for (const char character : trim(value)) {
        if (std::isspace(static_cast<unsigned char>(character))) {
            pending_space = !result.empty();
        } else {
            if (pending_space)
                result.push_back(' ');
            result.push_back(character);
            pending_space = false;
        }
    }
    return result;
}

using TypeDefinitions = std::map<std::string, std::string, std::less<>>;

std::optional<std::filesystem::path> resolve_include(std::string_view name,
    const std::filesystem::path& parent,
    const std::vector<std::filesystem::path>& include_directories)
{
    const auto local = parent / name;
    if (std::filesystem::exists(local))
        return local;
    for (const auto& directory : include_directories) {
        const auto candidate = directory / name;
        if (std::filesystem::exists(candidate))
            return candidate;
    }
    return std::nullopt;
}

void collect_definition_source(const std::filesystem::path& path,
    const std::vector<std::filesystem::path>& include_directories,
    std::set<std::filesystem::path>& visited, std::string& result)
{
    const auto normalized = std::filesystem::weakly_canonical(path);
    if (!visited.insert(normalized).second)
        return;
    const auto source = remove_comments(read_file(normalized));
    const std::regex includes { R"(#\s*include\s*[<"]([^>"]+\.defs)[>"])" };
    for (std::sregex_iterator
             iterator { source.begin(), source.end(), includes },
        end;
        iterator != end; ++iterator) {
        if (const auto included = resolve_include((*iterator)[1].str(),
                normalized.parent_path(), include_directories)) {
            collect_definition_source(
                *included, include_directories, visited, result);
        }
    }
    result.append(source);
    result.push_back('\n');
}

TypeDefinitions parse_type_definitions(std::string_view source)
{
    TypeDefinitions result;
    std::string text;
    std::istringstream lines { std::string { source } };
    std::string line;
    while (std::getline(lines, line)) {
        if (!trim(line).starts_with('#')) {
            text.append(line);
            text.push_back('\n');
        }
    }
    const std::regex definition {
        R"(\btype\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+);)"
    };
    for (std::sregex_iterator iterator { text.begin(), text.end(), definition },
        end;
        iterator != end; ++iterator) {
        auto expression = normalize_whitespace((*iterator)[2].str());
        for (const std::string_view transform :
            { "ctype", "intran", "outtran", "destructor", "cusertype" }) {
            const auto marker = " " + std::string { transform };
            const auto position = expression.find(marker);
            if (position == std::string::npos)
                continue;
            auto colon = position + marker.size();
            while (
                colon < expression.size() &&
                std::isspace(static_cast<unsigned char>(expression[colon]))) {
                ++colon;
            }
            if (colon < expression.size() && expression[colon] == ':') {
                expression.resize(position);
            }
        }
        result[(*iterator)[1].str()] = normalize_whitespace(expression);
    }
    return result;
}

struct ResolvedWireType {
    WireType type { WireType::Unknown };
    std::uint32_t size { };
    std::uint32_t count_prefix_size { };
    std::uint32_t element_size { };
};

ResolvedWireType resolve_wire_type(std::string expression,
    const TypeDefinitions& definitions, std::set<std::string>& resolving)
{
    expression = normalize_whitespace(expression);
    if (const auto comma = expression.find(','); comma != std::string::npos) {
        expression.resize(comma);
    }
    if (const auto equals = expression.find('='); equals != std::string::npos) {
        expression.resize(equals);
    }
    expression = normalize_whitespace(expression);
    if (const auto alias = definitions.find(expression);
        alias != definitions.end()) {
        if (!resolving.insert(expression).second)
            return { };
        const auto result =
            resolve_wire_type(alias->second, definitions, resolving);
        resolving.erase(expression);
        return result;
    }
    if (expression == "int" || expression == "unsigned" ||
        expression == "MACH_MSG_TYPE_INTEGER_32" ||
        expression == "MACH_MSG_TYPE_BOOLEAN" ||
        expression == "MACH_MSG_TYPE_PORT_NAME") {
        return { WireType::Scalar, 4 };
    }
    if (expression == "MACH_MSG_TYPE_INTEGER_64") {
        return { WireType::Scalar, 8 };
    }
    if (expression == "MACH_MSG_TYPE_INTEGER_16") {
        return { WireType::Scalar, 2 };
    }
    if (expression == "MACH_MSG_TYPE_INTEGER_8" ||
        expression == "MACH_MSG_TYPE_CHAR" ||
        expression == "MACH_MSG_TYPE_BYTE") {
        return { WireType::Scalar, 1 };
    }
    if (expression.find("polymorphic") != std::string::npos ||
        (expression.starts_with("MACH_MSG_TYPE_") &&
            expression != "MACH_MSG_TYPE_PORT_NAME")) {
        return { WireType::Port, 4 };
    }
    std::smatch match;
    if (std::regex_match(
            expression, match, std::regex { R"(c_string\[([0-9]+)\])" })) {
        return { WireType::FixedInline,
            static_cast<std::uint32_t>(std::stoul(match[1].str())), 0U, 1U };
    }
    if (std::regex_match(expression, match,
            std::regex { R"(c_string\[\*:\s*([0-9]+)\])" })) {
        // The Darwin 8 MIG wire representation used by the target firmware
        // reserves one natural_t before every variable c_string count.  The
        // iPhoneOS 1.0 IOKit stubs therefore place a first request string at
        // NDR + 4 (reserved) + 4 (count), rather than immediately after NDR.
        return { WireType::VariableInline,
            static_cast<std::uint32_t>(std::stoul(match[1].str())), 4U, 1U };
    }
    const std::regex aggregate {
        R"((\^?)\s*(array|struct)\s*\[([^\]]*)\]\s+of\s+(.+))"
    };
    if (std::regex_match(expression, match, aggregate)) {
        auto element =
            resolve_wire_type(match[4].str(), definitions, resolving);
        const auto pointer = match[1].matched && !match[1].str().empty();
        const auto count_text = normalize_whitespace(match[3].str());
        if (pointer || count_text.empty()) {
            return { element.type == WireType::Port ? WireType::OutOfLinePorts
                                                    : WireType::OutOfLine,
                0U, 0U, element.size };
        }
        if (count_text.starts_with("*:")) {
            const auto count_value = count_text.substr(2);
            if (!std::ranges::all_of(count_value, [](char character) {
                    return std::isdigit(static_cast<unsigned char>(character));
                })) {
                return { WireType::VariableInline, 0U, 0U, element.size };
            }
            const auto count =
                static_cast<std::uint32_t>(std::stoul(count_value));
            return { WireType::VariableInline, count * element.size, 0U,
                element.size };
        }
        if (!std::ranges::all_of(count_text, [](char character) {
                return std::isdigit(static_cast<unsigned char>(character));
            })) {
            return { WireType::FixedInline, 0U, 0U, element.size };
        }
        const auto count = static_cast<std::uint32_t>(std::stoul(count_text));
        return { WireType::FixedInline, count * element.size, 0U,
            element.size };
    }
    return { };
}

void resolve_routine_types(
    std::vector<Routine>& routines, const TypeDefinitions& definitions)
{
    for (auto& routine : routines) {
        for (auto& argument : routine.arguments) {
            std::set<std::string> resolving;
            const auto resolved =
                resolve_wire_type(argument.type, definitions, resolving);
            argument.wire_type = resolved.type;
            argument.wire_size = resolved.size;
            argument.count_prefix_size = resolved.count_prefix_size;
            argument.element_size = resolved.element_size;
        }
    }
}

constexpr std::uint32_t header_remote_port_offset = 8;
constexpr std::uint32_t header_local_port_offset = 12;
constexpr std::uint32_t descriptor_base = 28;
constexpr std::uint32_t descriptor_size = 12;
constexpr std::uint32_t simple_request_base = 32;
constexpr std::uint32_t simple_reply_base = 36;

constexpr std::uint32_t align_inline(std::uint32_t offset)
{
    constexpr std::uint32_t natural_alignment = 4;
    return (offset + natural_alignment - 1U) & ~(natural_alignment - 1U);
}

bool descriptor_type(WireType type)
{
    return type == WireType::Port || type == WireType::OutOfLine ||
           type == WireType::OutOfLinePorts;
}

bool request_argument(const Routine::Argument& argument)
{
    return argument.direction == Routine::Argument::Direction::In ||
           argument.direction == Routine::Argument::Direction::InOut;
}

bool reply_argument(const Routine::Argument& argument)
{
    return argument.direction == Routine::Argument::Direction::Out ||
           argument.direction == Routine::Argument::Direction::InOut;
}

void assign_wire_offsets(std::vector<Routine>& routines)
{
    for (auto& routine : routines) {
        std::size_t request_descriptors = 0;
        std::size_t reply_descriptors = 0;
        for (std::size_t index = 1; index < routine.arguments.size(); ++index) {
            const auto& argument = routine.arguments[index];
            if (argument.attributes == "ServerAuditToken" ||
                argument.attributes == "sreplyport") {
                continue;
            }
            if (descriptor_type(argument.wire_type)) {
                if (request_argument(argument))
                    ++request_descriptors;
                if (reply_argument(argument))
                    ++reply_descriptors;
            }
        }
        if (!routine.arguments.empty() &&
            request_argument(routine.arguments[0])) {
            routine.arguments[0].request_offset = header_remote_port_offset;
        }
        std::uint32_t request_cursor =
            request_descriptors == 0
                ? simple_request_base
                : descriptor_base +
                      static_cast<std::uint32_t>(request_descriptors) *
                          descriptor_size +
                      8U;
        std::uint32_t reply_cursor =
            reply_descriptors == 0
                ? simple_reply_base
                : descriptor_base +
                      static_cast<std::uint32_t>(reply_descriptors) *
                          descriptor_size +
                      8U;
        std::size_t request_descriptor = 0;
        std::size_t reply_descriptor = 0;
        bool request_fixed = true;
        bool reply_fixed = true;
        for (std::size_t index = 1; index < routine.arguments.size(); ++index) {
            auto& argument = routine.arguments[index];
            if (argument.attributes == "ServerAuditToken")
                continue;
            if (argument.attributes == "sreplyport") {
                argument.request_offset = header_local_port_offset;
                continue;
            }
            if (descriptor_type(argument.wire_type)) {
                if (request_argument(argument)) {
                    argument.request_offset =
                        descriptor_base +
                        static_cast<std::uint32_t>(request_descriptor++) *
                            descriptor_size;
                    if ((argument.wire_type == WireType::OutOfLine ||
                            argument.wire_type == WireType::OutOfLinePorts) &&
                        argument.element_size != 0) {
                        request_cursor = align_inline(request_cursor);
                        argument.request_count_offset = request_cursor;
                        request_cursor += 4U;
                    }
                }
                if (reply_argument(argument)) {
                    argument.reply_offset =
                        descriptor_base +
                        static_cast<std::uint32_t>(reply_descriptor++) *
                            descriptor_size;
                    if ((argument.wire_type == WireType::OutOfLine ||
                            argument.wire_type == WireType::OutOfLinePorts) &&
                        argument.element_size != 0) {
                        reply_cursor = align_inline(reply_cursor);
                        argument.reply_count_offset = reply_cursor;
                        reply_cursor += 4U;
                    }
                }
                continue;
            }
            const auto variable =
                argument.wire_type == WireType::VariableInline;
            const auto count_inout =
                argument.type.find("CountInOut") != std::string::npos;
            if (!request_argument(argument) && count_inout && request_fixed) {
                request_cursor = align_inline(request_cursor);
                argument.request_count_offset = request_cursor;
                request_cursor += 4U;
            }
            if (request_argument(argument) && request_fixed &&
                argument.wire_type != WireType::Unknown) {
                request_cursor = align_inline(request_cursor);
                if (variable) {
                    request_cursor += argument.count_prefix_size;
                    argument.request_count_offset = request_cursor;
                    request_cursor += 4U;
                }
                argument.request_offset = request_cursor;
                request_cursor += argument.wire_size;
                if (variable)
                    request_fixed = false;
            }
            if (reply_argument(argument) && reply_fixed &&
                argument.wire_type != WireType::Unknown) {
                reply_cursor = align_inline(reply_cursor);
                if (variable) {
                    reply_cursor += argument.count_prefix_size;
                    argument.reply_count_offset = reply_cursor;
                    reply_cursor += 4U;
                }
                argument.reply_offset = reply_cursor;
                reply_cursor += argument.wire_size;
                if (variable)
                    reply_fixed = false;
            }
        }
    }
}

std::string cpp_string(std::string_view value)
{
    std::string result;
    for (const char character : value) {
        if (character == '\\' || character == '"')
            result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

bool cpp_keyword(std::string_view value)
{
    static constexpr std::string_view keywords[] { "alignas", "alignof", "and",
        "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
        "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl",
        "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
        "continue", "co_await", "co_return", "co_yield", "decltype", "default",
        "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
        "export", "extern", "false", "float", "for", "friend", "goto", "if",
        "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
        "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private",
        "protected", "public", "register", "reinterpret_cast", "requires",
        "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while",
        "xor", "xor_eq", "final", "override", "import", "module",
        "transaction_safe" };
    for (const auto keyword : keywords) {
        if (keyword == value)
            return true;
    }
    return false;
}

std::string cpp_identifier(std::string_view value)
{
    return cpp_keyword(value) ? "mig_" + std::string { value }
                              : std::string { value };
}

std::vector<Routine::Argument> parse_arguments(std::string_view body)
{
    std::vector<Routine::Argument> result;
    std::size_t start = 0;
    while (start <= body.size()) {
        const auto end = body.find(';', start);
        const auto declaration = trim(body.substr(start,
            end == std::string_view::npos ? body.size() - start : end - start));
        if (!declaration.empty()) {
            const auto colon = declaration.find(':');
            if (colon == std::string_view::npos) {
                throw std::runtime_error {
                    "cannot parse MIG argument declaration: " +
                    std::string { declaration }
                };
            }
            auto left = trim(declaration.substr(0, colon));
            Routine::Argument::Direction direction =
                Routine::Argument::Direction::In;
            const auto consume_direction = [&](std::string_view keyword) {
                if (!left.starts_with(keyword) ||
                    (left.size() != keyword.size() &&
                        !std::isspace(static_cast<unsigned char>(
                            left[keyword.size()])))) {
                    return false;
                }
                left = trim(left.substr(keyword.size()));
                return true;
            };
            if (consume_direction("inout")) {
                direction = Routine::Argument::Direction::InOut;
            } else if (consume_direction("out")) {
                direction = Routine::Argument::Direction::Out;
            } else {
                static_cast<void>(consume_direction("in"));
            }
            const auto qualified_name = normalize_whitespace(left);
            const auto qualifier_end = qualified_name.find_last_of(' ');
            const auto name = qualifier_end == std::string::npos
                                  ? qualified_name
                                  : qualified_name.substr(qualifier_end + 1);
            const auto attributes =
                qualifier_end == std::string::npos
                    ? std::string { }
                    : qualified_name.substr(0, qualifier_end);
            const auto type =
                normalize_whitespace(declaration.substr(colon + 1));
            if (!valid_identifier(name) || type.empty()) {
                throw std::runtime_error {
                    "invalid MIG argument declaration: " +
                    std::string { declaration }
                };
            }
            result.push_back({ name, type, attributes, direction });
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return result;
}

std::string_view parenthesized_body(
    std::string_view source, std::size_t body_start)
{
    std::size_t depth = 1;
    for (std::size_t index = body_start; index < source.size(); ++index) {
        if (source[index] == '(') {
            ++depth;
        } else if (source[index] == ')') {
            if (--depth == 0) {
                return source.substr(body_start, index - body_start);
            }
        }
    }
    throw std::runtime_error { "unterminated MIG routine argument list" };
}

Arguments parse_arguments(int argc, char** argv)
{
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument { argv[index] };
        if (index + 1 >= argc) {
            throw std::runtime_error { "missing value after " +
                                       std::string { argument } };
        }
        const std::string value { argv[++index] };
        if (argument == "--input") {
            result.input = value;
        } else if (argument == "--output") {
            result.output = value;
        } else if (argument == "--namespace") {
            result.cpp_namespace = value;
        } else if (argument == "--include-dir") {
            result.include_directories.emplace_back(value);
        } else if (argument == "--type-def") {
            result.type_definitions.emplace_back(value);
        } else if (argument == "--type-alias") {
            const auto equals = value.find('=');
            if (equals == std::string::npos ||
                !valid_identifier(value.substr(0, equals)) ||
                value.substr(equals + 1).empty()) {
                throw std::runtime_error { "invalid --type-alias " + value };
            }
            result.type_aliases.emplace_back(
                value.substr(0, equals), value.substr(equals + 1));
        } else {
            throw std::runtime_error { "unknown argument " +
                                       std::string { argument } };
        }
    }
    if (result.input.empty() || result.output.empty() ||
        !valid_identifier(result.cpp_namespace)) {
        throw std::runtime_error {
            "usage: mig_id_gen --input FILE --output FILE --namespace NAME "
            "[--include-dir DIR] [--type-def FILE] [--type-alias NAME=TYPE]"
        };
    }
    return result;
}

std::pair<std::string, std::uint32_t> parse_subsystem(std::string_view source)
{
    const auto start = source.find("subsystem");
    if (start == std::string_view::npos) {
        throw std::runtime_error { "MIG subsystem declaration is missing" };
    }
    const auto end = source.find(';', start);
    if (end == std::string_view::npos) {
        throw std::runtime_error {
            "MIG subsystem declaration is unterminated"
        };
    }
    const std::string declaration { source.substr(start, end - start + 1) };
    const std::regex pattern { R"(([A-Za-z_][A-Za-z0-9_]*)\s+([0-9]+)\s*;)" };
    std::smatch match;
    if (!std::regex_search(declaration, match, pattern)) {
        throw std::runtime_error { "cannot parse MIG subsystem name/base" };
    }
    return { match[1].str(),
        static_cast<std::uint32_t>(std::stoul(match[2].str())) };
}

std::vector<Routine> parse_routines(std::string_view source, std::uint32_t base)
{
    const auto subsystem = source.find("subsystem");
    const auto body = source.find(';', subsystem);
    if (body == std::string_view::npos)
        return { };
    const std::string text { source.substr(body + 1) };
    const std::regex token {
        R"(\b(?:routine|simpleroutine)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(|\b(skip)\s*;)"
    };
    std::vector<Routine> result;
    auto identifier = base;
    for (std::sregex_iterator iterator { text.begin(), text.end(), token }, end;
        iterator != end; ++iterator, ++identifier) {
        if ((*iterator)[1].matched) {
            const auto body_start = static_cast<std::size_t>(
                iterator->position() + iterator->length());
            result.push_back({ (*iterator)[1].str(), identifier,
                parse_arguments(parenthesized_body(text, body_start)) });
        }
    }
    if (result.empty()) {
        throw std::runtime_error { "MIG definition contains no routines" };
    }
    return result;
}

void write_header(const Arguments& arguments, std::string_view subsystem,
    std::uint32_t base, const std::vector<Routine>& routines)
{
    std::filesystem::create_directories(arguments.output.parent_path());
    std::ofstream output { arguments.output };
    if (!output) {
        throw std::runtime_error { "cannot create " +
                                   arguments.output.string() };
    }
    output << "// Generated from " << arguments.input.filename().string()
           << "; do not edit.\n"
              "#pragma once\n\n"
              "#include <array>\n"
              "#include <cstdint>\n"
              "#include <span>\n"
              "#include <string_view>\n\n"
              "#include \"ilemu/xnu_mig_adapter.hpp\"\n\n"
              "namespace ilemu::xnu792::mig::"
           << arguments.cpp_namespace
           << " {\n\n"
              "inline constexpr std::string_view subsystem_name{\""
           << subsystem
           << "\"};\n"
              "inline constexpr std::uint32_t subsystem_base = "
           << base
           << "U;\n\n"
              "enum class Routine : std::uint32_t {\n";
    for (const auto& routine : routines) {
        output << "    " << cpp_identifier(routine.name) << " = "
               << routine.identifier << "U,\n";
    }
    output << "};\n\n";
    for (const auto& routine : routines) {
        const auto generated_name = cpp_identifier(routine.name);
        output << "inline constexpr std::array<ArgumentInfo, "
               << routine.arguments.size() << "> " << generated_name
               << "_arguments{{\n";
        for (const auto& argument : routine.arguments) {
            const char* direction = "ArgumentDirection::In";
            if (argument.direction == Routine::Argument::Direction::Out) {
                direction = "ArgumentDirection::Out";
            } else if (argument.direction ==
                       Routine::Argument::Direction::InOut) {
                direction = "ArgumentDirection::InOut";
            }
            const char* wire_type = "WireType::Unknown";
            switch (argument.wire_type) {
            case WireType::Scalar:
                wire_type = "WireType::Scalar";
                break;
            case WireType::Port:
                wire_type = "WireType::Port";
                break;
            case WireType::FixedInline:
                wire_type = "WireType::FixedInline";
                break;
            case WireType::VariableInline:
                wire_type = "WireType::VariableInline";
                break;
            case WireType::OutOfLine:
                wire_type = "WireType::OutOfLine";
                break;
            case WireType::OutOfLinePorts:
                wire_type = "WireType::OutOfLinePorts";
                break;
            case WireType::Unknown:
                break;
            }
            output << "    {\"" << cpp_string(argument.name) << "\", \""
                   << cpp_string(argument.type) << "\", \""
                   << cpp_string(argument.attributes) << "\", " << direction
                   << ", " << wire_type << ", " << argument.wire_size << "U, "
                   << argument.count_prefix_size << "U, "
                   << argument.element_size << "U, " << argument.request_offset
                   << "U, " << argument.reply_offset << "U, "
                   << argument.request_count_offset << "U, "
                   << argument.reply_count_offset << "U},\n";
        }
        output << "}};\n\n";
    }
    output << "struct Descriptor {\n"
              "    Routine routine;\n"
              "    std::string_view name;\n"
              "    std::span<const ArgumentInfo> arguments;\n"
              "};\n\n"
              "inline constexpr std::array<Descriptor, "
           << routines.size() << "> routines{{\n";
    for (const auto& routine : routines) {
        const auto generated_name = cpp_identifier(routine.name);
        output << "    {Routine::" << generated_name << ", \"" << routine.name
               << "\", std::span<const ArgumentInfo>{" << generated_name
               << "_arguments}},\n";
    }
    output << "}};\n\n"
              "constexpr std::uint32_t id(Routine routine) {\n"
              "    return static_cast<std::uint32_t>(routine);\n"
              "}\n\n"
              "}  // namespace ilemu::xnu792::mig::"
           << arguments.cpp_namespace << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto arguments = parse_arguments(argc, argv);
        const auto source = preprocess_kernel_server(
            remove_comments(read_file(arguments.input)));
        const auto [subsystem, base] = parse_subsystem(source);
        auto routines = parse_routines(source, base);
        std::set<std::filesystem::path> visited;
        std::string type_source;
        collect_definition_source(arguments.input,
            arguments.include_directories, visited, type_source);
        for (const auto& definition : arguments.type_definitions) {
            collect_definition_source(definition, arguments.include_directories,
                visited, type_source);
        }
        auto definitions = parse_type_definitions(type_source);
        for (const auto& [name, expression] : arguments.type_aliases) {
            definitions[name] = expression;
        }
        resolve_routine_types(routines, definitions);
        assign_wire_offsets(routines);
        write_header(arguments, subsystem, base, routines);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mig_id_gen: " << error.what() << '\n';
        return 1;
    }
}
