// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Classify conventional compositor shader inputs from their GLSL contract.

#include "graphics/gles_program_interface_profile.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ilemu {
namespace {

    struct Declaration {
        std::string_view storage;
        std::string_view type;
        std::string_view name;
    };

    struct LocalAssignment {
        std::string_view name;
        std::size_t expression_begin { };
        std::size_t expression_end { };
    };

    bool identifier_character(char value)
    {
        return std::isalnum(static_cast<unsigned char>(value)) != 0 ||
               value == '_';
    }

    std::vector<std::string_view> tokenize(std::string_view source)
    {
        std::vector<std::string_view> result;
        for (std::size_t offset = 0; offset < source.size();) {
            if (identifier_character(source[offset])) {
                const auto begin = offset++;
                while (offset < source.size() &&
                       identifier_character(source[offset])) {
                    ++offset;
                }
                result.push_back(source.substr(begin, offset - begin));
            } else if (!std::isspace(
                           static_cast<unsigned char>(source[offset]))) {
                result.push_back(source.substr(offset, 1U));
                ++offset;
            } else {
                ++offset;
            }
        }
        return result;
    }

    bool precision(std::string_view token)
    {
        return token == "highp" || token == "mediump" || token == "lowp";
    }

    std::vector<Declaration> declarations(
        const std::vector<std::string_view>& tokens)
    {
        std::vector<Declaration> result;
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            const auto storage = tokens[index];
            if (storage != "attribute" && storage != "uniform" &&
                storage != "varying") {
                continue;
            }
            auto cursor = index + 1U;
            if (cursor < tokens.size() && precision(tokens[cursor]))
                ++cursor;
            if (cursor >= tokens.size())
                continue;
            const auto type = tokens[cursor++];
            while (cursor < tokens.size() && tokens[cursor] != ";") {
                if (tokens[cursor] != "," && tokens[cursor] != "[" &&
                    tokens[cursor] != "]" &&
                    identifier_character(tokens[cursor].front())) {
                    result.push_back({ storage, type, tokens[cursor] });
                    while (cursor + 1U < tokens.size() &&
                           tokens[cursor + 1U] != "," &&
                           tokens[cursor + 1U] != ";") {
                        ++cursor;
                    }
                }
                ++cursor;
            }
            index = cursor;
        }
        return result;
    }

    std::vector<std::string_view> names(const std::vector<Declaration>& values,
        std::string_view storage, std::string_view type = { })
    {
        std::vector<std::string_view> result;
        for (const auto& value : values) {
            if (value.storage == storage &&
                (type.empty() || value.type == type)) {
                result.push_back(value.name);
            }
        }
        return result;
    }

    bool contains_sequence(const std::vector<std::string_view>& tokens,
        std::size_t begin, std::size_t end,
        std::span<const std::string_view> sequence)
    {
        if (sequence.empty() || end - begin < sequence.size())
            return false;
        return std::search(tokens.begin() + static_cast<std::ptrdiff_t>(begin),
                   tokens.begin() + static_cast<std::ptrdiff_t>(end),
                   sequence.begin(), sequence.end()) !=
               tokens.begin() + static_cast<std::ptrdiff_t>(end);
    }

    bool contains_sequence(const std::vector<std::string_view>& tokens,
        std::span<const std::string_view> sequence)
    {
        return contains_sequence(tokens, 0U, tokens.size(), sequence);
    }

    std::vector<LocalAssignment> local_vec4_assignments(
        const std::vector<std::string_view>& tokens)
    {
        std::vector<LocalAssignment> result;
        for (std::size_t index = 0; index + 3U < tokens.size(); ++index) {
            if (tokens[index] != "vec4" || tokens[index + 2U] != "=")
                continue;
            auto end = index + 3U;
            while (end < tokens.size() && tokens[end] != ";")
                ++end;
            result.push_back({ tokens[index + 1U], index + 3U, end });
            index = end;
        }
        return result;
    }

    std::optional<std::string_view> projective_sample_result(
        const std::vector<std::string_view>& tokens, std::string_view sampler,
        std::string_view varying)
    {
        const std::array<std::string_view, 6> call { "texture2DProj", "(",
            sampler, ",", varying, ")" };
        const auto found =
            std::search(tokens.begin(), tokens.end(), call.begin(), call.end());
        if (found == tokens.end())
            return std::nullopt;
        auto index =
            static_cast<std::size_t>(std::distance(tokens.begin(), found));
        while (index != 0U && tokens[index] != "=" && tokens[index] != ";") {
            --index;
        }
        if (tokens[index] != "=" || index == 0U)
            return std::nullopt;
        return tokens[index - 1U];
    }

    std::string_view assigned_from(const std::vector<std::string_view>& tokens,
        const std::vector<LocalAssignment>& assignments,
        std::string_view source)
    {
        for (const auto& assignment : assignments) {
            const std::array sequence { source };
            if (contains_sequence(tokens, assignment.expression_begin,
                    assignment.expression_end, sequence)) {
                return assignment.name;
            }
        }
        return { };
    }

    GlesFragmentOperation classify_fragment_operation(
        const std::vector<std::string_view>& tokens,
        std::string_view color_varying, std::string_view sampled,
        const std::string& fragment)
    {
        const auto assignments = local_vec4_assignments(tokens);
        auto source = assigned_from(tokens, assignments, color_varying);
        if (source.empty()) {
            for (const auto output : { std::string_view { "gl_FragColor" },
                     std::string_view { "gl_FragData" } }) {
                source = assigned_from(tokens, assignments, output);
                if (!source.empty())
                    break;
            }
        }
        const auto destination = assigned_from(tokens, assignments, sampled);
        if (source.empty() || destination.empty())
            return GlesFragmentOperation::TextureEnvironment;

        // Match the complete helper contract, including its parameters and
        // call site. Names are guest supplied; only the expression determines
        // the operation.
        static const std::regex luminance_source_over(
            R"((?:(?:lowp|mediump|highp)\s+)?vec4\s+(\w+)\s*\(\s*(?:(?:lowp|mediump|highp)\s+)?vec4\s+(\w+))"
            R"(\s*,\s*(?:(?:lowp|mediump|highp)\s+)?vec4\s+(\w+)\s*\)\s*\{)"
            R"(\s*(?:(?:lowp|mediump|highp)\s+)?float\s+(\w+)\s*=\s*(1(?:\.0*)?\s*-\s*)?dot\s*\(\s*\3\.rgb\s*,\s*vec3\s*\()"
            R"(\s*0?\.2125\s*,\s*0?\.7154\s*,\s*0?\.0721\s*\)\s*\)\s*;\s*\2\s*=\s*\2\s*\*\s*\(\s*\4\s*\*)"
            R"(\s*\4\s*\)\s*\*\s*\(\s*\4\s*\*\s*\4\s*\)\s*;\s*return\s+\3\s*\*\s*\(\s*1(?:\.0*)?\s*-)"
            R"(\s*\2\.a\s*\)\s*\+\s*\2\s*;\s*\})");
        std::smatch luminance_match;
        if (std::regex_search(fragment, luminance_match, luminance_source_over)) {
            const auto function = luminance_match[1].str();
            const std::array call { std::string_view { function },
                std::string_view { "(" }, source, std::string_view { "," },
                destination, std::string_view { ")" } };
            if (contains_sequence(tokens, call))
                return luminance_match[5].matched
                           ? GlesFragmentOperation::InverseLuminanceSourceOver
                           : GlesFragmentOperation::LuminanceSourceOver;
        }

        for (const auto& assignment : assignments) {
            const auto result = assignment.name;
            const auto screen = std::to_array<std::string_view>({
                destination, "*", "(", "1", ".", "-", source, ".", "a", ")",
                "+", source, "*", "(", "1", ".", "-", destination,
                ".", "a", ")", "+", destination, "*", source
            });
            if (contains_sequence(tokens, assignment.expression_begin,
                    assignment.expression_end, screen))
                return GlesFragmentOperation::Screen;
            const auto linear_light = std::to_array<std::string_view>({
                result, ".", "rgb", "+", "=", destination, ".", "rgb",
                "*", source, ".", "a", "-", destination, ".", "a",
                "*", "(", source, ".", "a", "-", "2", ".", "*",
                source, ".", "rgb", ")"
            });
            const auto alpha_product = std::to_array<std::string_view>({
                result, ".", "a", "+", "=", destination, ".", "a",
                "*", source, ".", "a"
            });
            const auto darken_base = std::to_array<std::string_view>({
                destination, "*", "(", "1", ".", "-", source, ".", "a", ")",
                "+", source, "*", "(", "1", ".", "-", destination,
                ".", "a", ")"
            });
            const auto darken_rgb = std::to_array<std::string_view>({
                result, ".", "rgb", "+", "=", "min", "(", destination,
                ".", "rgb", "*", source, ".", "a", ",", source, ".",
                "rgb", "*", destination, ".", "a", ")"
            });
            if (contains_sequence(tokens, assignment.expression_begin,
                    assignment.expression_end, darken_base) &&
                contains_sequence(tokens, darken_rgb) &&
                contains_sequence(tokens, alpha_product)) {
                return GlesFragmentOperation::Darken;
            }
            const auto overlay_expression =
                std::string { result } + ".rgb += mix(2.*" +
                std::string { destination } + ".rgb*" +
                std::string { source } + ".rgb, 2.*(" +
                std::string { source } + ".rgb*" +
                std::string { destination } + ".a + " +
                std::string { destination } + ".rgb*(" +
                std::string { source } + ".a - " +
                std::string { source } + ".rgb)) - " +
                std::string { source } + ".a*" +
                std::string { destination } + ".a, step(.5*" +
                std::string { destination } + ".a, " +
                std::string { destination } + ".rgb))";
            const auto overlay_rgb = tokenize(overlay_expression);
            if (contains_sequence(tokens, assignment.expression_begin,
                    assignment.expression_end, darken_base) &&
                contains_sequence(tokens, overlay_rgb) &&
                contains_sequence(tokens, alpha_product)) {
                return GlesFragmentOperation::Overlay;
            }
            if (contains_sequence(tokens, linear_light) &&
                contains_sequence(tokens, alpha_product))
                return GlesFragmentOperation::LinearLight;
            const std::array source_plus_destination { source,
                std::string_view { "+" }, destination };
            const std::array destination_plus_source { destination,
                std::string_view { "+" }, source };
            const auto plus =
                contains_sequence(tokens, assignment.expression_begin,
                    assignment.expression_end, source_plus_destination) ||
                contains_sequence(tokens, assignment.expression_begin,
                    assignment.expression_end, destination_plus_source);
            const std::array complement { result, std::string_view { "." },
                std::string_view { "rgb" }, std::string_view { "=" }, result,
                std::string_view { "." }, std::string_view { "a" },
                std::string_view { "-" }, result, std::string_view { "." },
                std::string_view { "rgb" } };
            const std::array clamp { std::string_view { "clamp" },
                std::string_view { "(" }, result, std::string_view { "," } };
            if (plus && contains_sequence(tokens, complement) &&
                contains_sequence(tokens, clamp)) {
                return GlesFragmentOperation::PlusLighter;
            }

            const std::array dodge { destination, std::string_view { "." },
                std::string_view { "rgb" }, std::string_view { "*" }, source,
                std::string_view { "." }, std::string_view { "a" },
                std::string_view { "*" }, source, std::string_view { "." },
                std::string_view { "a" }, std::string_view { "/" },
                std::string_view { "max" }, std::string_view { "(" }, source,
                std::string_view { "." }, std::string_view { "a" },
                std::string_view { "-" }, source, std::string_view { "." },
                std::string_view { "rgb" } };
            const std::array minimum { result, std::string_view { "." },
                std::string_view { "rgb" }, std::string_view { "=" },
                std::string_view { "min" }, std::string_view { "(" }, result,
                std::string_view { "." }, std::string_view { "rgb" },
                std::string_view { "," }, result, std::string_view { "." },
                std::string_view { "a" } };
            if (contains_sequence(tokens, dodge) &&
                contains_sequence(tokens, minimum)) {
                return GlesFragmentOperation::ColorDodge;
            }
            const auto burn_expression = std::string { result } +
                ".rgb += step(.005, " + std::string { source } +
                ".rgb) * (" + std::string { destination } + ".a*" +
                std::string { source } + ".a - " + std::string { source } +
                ".a*" + std::string { source } + ".a*(" +
                std::string { destination } + ".a - " +
                std::string { destination } + ".rgb)/max(" +
                std::string { source } + ".rgb, .005))";
            const auto burn_rgb = tokenize(burn_expression);
            if (contains_sequence(tokens, assignment.expression_begin,
                    assignment.expression_end, darken_base) &&
                contains_sequence(tokens, burn_rgb) &&
                contains_sequence(tokens, alpha_product) &&
                contains_sequence(tokens, minimum)) {
                return GlesFragmentOperation::ColorBurn;
            }
        }
        return GlesFragmentOperation::TextureEnvironment;
    }

    std::optional<std::string_view> find_position_attribute(
        const std::vector<std::string_view>& tokens,
        const std::vector<std::string_view>& attributes)
    {
        for (std::size_t index = 0; index + 2U < tokens.size(); ++index) {
            if (tokens[index] != "gl_Position" || tokens[index + 1U] != "=")
                continue;
            std::optional<std::string_view> result;
            for (auto cursor = index + 2U;
                cursor < tokens.size() && tokens[cursor] != ";"; ++cursor) {
                if (std::find(attributes.begin(), attributes.end(),
                        tokens[cursor]) == attributes.end()) {
                    continue;
                }
                if (result && *result != tokens[cursor])
                    return std::nullopt;
                result = tokens[cursor];
            }
            return result;
        }
        return std::nullopt;
    }

    GlesProgramInterfaceProfile::MatrixOrder matrix_order_in_statement(
        const std::vector<std::string_view>& tokens, std::string_view input,
        std::string_view matrix)
    {
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            if (tokens[index] != input)
                continue;
            auto begin = index;
            while (begin != 0U && tokens[begin - 1U] != ";")
                --begin;
            auto end = index;
            while (end < tokens.size() && tokens[end] != ";")
                ++end;
            const auto matrix_position =
                std::find(tokens.begin() + static_cast<std::ptrdiff_t>(begin),
                    tokens.begin() + static_cast<std::ptrdiff_t>(end), matrix);
            if (matrix_position ==
                tokens.begin() + static_cast<std::ptrdiff_t>(end)) {
                continue;
            }
            const auto matrix_index = static_cast<std::size_t>(
                std::distance(tokens.begin(), matrix_position));
            const auto first = std::min(index, matrix_index);
            const auto last = std::max(index, matrix_index);
            if (std::find(tokens.begin() + static_cast<std::ptrdiff_t>(first),
                    tokens.begin() + static_cast<std::ptrdiff_t>(last), "*") ==
                tokens.begin() + static_cast<std::ptrdiff_t>(last)) {
                continue;
            }
            return index < matrix_index ? GlesProgramInterfaceProfile::
                                              MatrixOrder::VectorTimesMatrix
                                        : GlesProgramInterfaceProfile::
                                              MatrixOrder::MatrixTimesVector;
        }
        return GlesProgramInterfaceProfile::MatrixOrder::None;
    }

    GlesProgramInterfaceProfile::MatrixOrder matrix_order_in_texture_call(
        const std::vector<std::string_view>& tokens, std::string_view sampler,
        std::string_view matrix)
    {
        for (std::size_t index = 0; index + 1U < tokens.size(); ++index) {
            if (tokens[index] != "texture2D" &&
                tokens[index] != "texture2DRect") {
                continue;
            }
            auto end = index + 1U;
            auto depth = 0;
            for (; end < tokens.size(); ++end) {
                if (tokens[end] == "(")
                    ++depth;
                else if (tokens[end] == ")" && --depth == 0)
                    break;
            }
            const auto sampler_position =
                std::find(tokens.begin() + static_cast<std::ptrdiff_t>(index),
                    tokens.begin() + static_cast<std::ptrdiff_t>(end), sampler);
            const auto matrix_position =
                std::find(tokens.begin() + static_cast<std::ptrdiff_t>(index),
                    tokens.begin() + static_cast<std::ptrdiff_t>(end), matrix);
            if (sampler_position ==
                    tokens.begin() + static_cast<std::ptrdiff_t>(end) ||
                matrix_position ==
                    tokens.begin() + static_cast<std::ptrdiff_t>(end)) {
                continue;
            }
            const auto matrix_index = static_cast<std::size_t>(
                std::distance(tokens.begin(), matrix_position));
            const auto star =
                std::find(tokens.begin() + static_cast<std::ptrdiff_t>(index),
                    tokens.begin() + static_cast<std::ptrdiff_t>(end), "*");
            if (star == tokens.begin() + static_cast<std::ptrdiff_t>(end))
                continue;
            return static_cast<std::size_t>(
                       std::distance(tokens.begin(), star)) < matrix_index
                       ? GlesProgramInterfaceProfile::MatrixOrder::
                             VectorTimesMatrix
                       : GlesProgramInterfaceProfile::MatrixOrder::
                             MatrixTimesVector;
        }
        return GlesProgramInterfaceProfile::MatrixOrder::None;
    }

    GlesProgramInterfaceProfile::MatrixOrder wrapped_texture_matrix_order(
        const std::vector<std::string_view>& tokens)
    {
        for (std::size_t index = 0; index + 1U < tokens.size(); ++index) {
            if (tokens[index] != "texture2D" &&
                tokens[index] != "texture2DRect") {
                continue;
            }
            auto end = index + 1U;
            auto depth = 0;
            for (; end < tokens.size(); ++end) {
                if (tokens[end] == "(")
                    ++depth;
                else if (tokens[end] == ")" && --depth == 0)
                    break;
            }
            const auto star =
                std::find(tokens.begin() + static_cast<std::ptrdiff_t>(index),
                    tokens.begin() + static_cast<std::ptrdiff_t>(end), "*");
            if (star == tokens.begin() + static_cast<std::ptrdiff_t>(end))
                continue;
            const auto star_index =
                static_cast<std::size_t>(std::distance(tokens.begin(), star));
            if (star_index > index && tokens[star_index - 1U] == ")") {
                return GlesProgramInterfaceProfile::MatrixOrder::
                    VectorTimesMatrix;
            }
            if (star_index + 1U < end && tokens[star_index + 1U] == "(") {
                return GlesProgramInterfaceProfile::MatrixOrder::
                    MatrixTimesVector;
            }
        }
        return GlesProgramInterfaceProfile::MatrixOrder::None;
    }

} // namespace

GlesProgramInterfaceProfile GlesProgramInterfaceProfile::from_sources(
    std::string_view vertex, std::string_view fragment)
{
    GlesProgramInterfaceProfile result;
    result.filter = GlesFilterProfile::from_sources(vertex, fragment);
    // A constant zero source is used for empty compositor content. Preserve
    // its transparent output even when the vertex color is opaque. Restrict
    // the contract to a complete main body, so later writes cannot invalidate
    // the constant expression.
    const auto uncommented = std::regex_replace(std::string { fragment },
        std::regex(R"(/\*[\s\S]*?\*/|//[^\n]*)"), "");
    const std::regex transparent_main(
        R"(void\s+main\s*\(\s*(?:void)?\s*\)\s*\{\s*(?:(?:lowp|mediump|highp)\s+)?vec4\s+(\w+)\s*=)"
        R"(\s*vec4\s*\(\s*0(?:\.0*)?\s*\)\s*;\s*(?:gl_FragColor|gl_FragData\s*\[\s*0\s*\])\s*=\s*(?:\w+)"
        R"(\s*\*\s*\1|\1\s*\*\s*\w+|\1)\s*;\s*\})");
    result.transparent_output = std::regex_search(uncommented, transparent_main);
    const auto vertex_tokens = tokenize(vertex);
    const auto fragment_tokens = tokenize(fragment);
    const auto vertex_declarations = declarations(vertex_tokens);
    const auto fragment_declarations = declarations(fragment_tokens);
    const auto attributes = names(vertex_declarations, "attribute");
    if (const auto position =
            find_position_attribute(vertex_tokens, attributes)) {
        result.position_attribute = *position;
    }

    if (vertex.find("vertex_color0") != std::string_view::npos) {
        result.color_attribute = "vertex_color0";
        result.color_varying = "color0";
    }
    if (fragment.find("gl_FragData[0]") != std::string_view::npos)
        result.fragment_output = "gl_FragData[0]";

    const auto vertex_vec4_uniforms =
        names(vertex_declarations, "uniform", "vec4");
    const auto fragment_varyings = names(fragment_declarations, "varying");
    const auto samplers_2d =
        names(fragment_declarations, "uniform", "sampler2D");
    const auto samplers_rect =
        names(fragment_declarations, "uniform", "sampler2DRect");
    std::size_t projected_index { };
    for (std::size_t index = 0;
        index + 5U < fragment_tokens.size() &&
        projected_index < result.projected_texture_inputs.size();
        ++index) {
        if (fragment_tokens[index] != "texture2DProj" ||
            fragment_tokens[index + 1U] != "(" ||
            fragment_tokens[index + 3U] != "," ||
            fragment_tokens[index + 5U] != ")") {
            continue;
        }
        const auto sampler = fragment_tokens[index + 2U];
        const auto varying = fragment_tokens[index + 4U];
        const auto sampler_2d =
            std::find(samplers_2d.begin(), samplers_2d.end(), sampler);
        const auto sampler_rect =
            std::find(samplers_rect.begin(), samplers_rect.end(), sampler);
        if ((sampler_2d == samplers_2d.end() &&
                sampler_rect == samplers_rect.end()) ||
            std::find(fragment_varyings.begin(), fragment_varyings.end(),
                varying) == fragment_varyings.end()) {
            continue;
        }
        for (const auto transform : vertex_vec4_uniforms) {
            const std::array projection { varying, std::string_view { "=" },
                std::string_view { "vec3" }, std::string_view { "(" },
                std::string_view { "gl_Position" }, std::string_view { "." },
                std::string_view { "xy" }, std::string_view { "*" }, transform,
                std::string_view { "." }, std::string_view { "xy" },
                std::string_view { "+" }, transform, std::string_view { "." },
                std::string_view { "zw" }, std::string_view { "*" },
                std::string_view { "gl_Position" }, std::string_view { "." },
                std::string_view { "w" }, std::string_view { "," },
                std::string_view { "gl_Position" }, std::string_view { "." },
                std::string_view { "w" }, std::string_view { ")" } };
            if (!contains_sequence(vertex_tokens, projection))
                continue;
            auto& input = result.projected_texture_inputs[projected_index++];
            input.sampler = sampler;
            input.varying = varying;
            input.transform_uniform = transform;
            input.rectangle = sampler_rect != samplers_rect.end();
            if (const auto sampled = projective_sample_result(
                    fragment_tokens, sampler, varying)) {
                result.fragment_operation = classify_fragment_operation(
                    fragment_tokens, result.color_varying, *sampled, uncommented);
            }
            break;
        }
    }

    const std::array<std::string_view, 4> fetched_pixel { "gl_LastFragData",
        "[", "0", "]" };
    if (contains_sequence(fragment_tokens, fetched_pixel)) {
        const auto assignments = local_vec4_assignments(fragment_tokens);
        const auto sampled =
            assigned_from(fragment_tokens, assignments, "gl_LastFragData");
        result.fragment_operation = classify_fragment_operation(
            fragment_tokens, result.color_varying, sampled, uncommented);
        result.framebuffer_fetch = result.fragment_operation !=
                                   GlesFragmentOperation::TextureEnvironment;
    }

    auto texture_attributes = attributes;
    std::erase(texture_attributes, result.position_attribute);
    std::erase(texture_attributes, result.color_attribute);
    const auto vertex_matrices = names(vertex_declarations, "uniform", "mat3");
    const auto fragment_matrices =
        names(fragment_declarations, "uniform", "mat3");
    if (texture_attributes.size() == 1U &&
        samplers_2d.size() + samplers_rect.size() == 1U) {
        auto& input = result.matrix_texture_inputs[0];
        input.attribute = texture_attributes.front();
        input.rectangle = samplers_2d.empty();
        input.sampler =
            input.rectangle ? samplers_rect.front() : samplers_2d.front();
        for (const auto matrix : vertex_matrices) {
            const auto order = matrix_order_in_statement(
                vertex_tokens, input.attribute, matrix);
            if (order != MatrixOrder::None) {
                input.vertex_matrix = matrix;
                input.vertex_matrix_order = order;
                break;
            }
        }
        for (const auto matrix : fragment_matrices) {
            auto order = matrix_order_in_texture_call(
                fragment_tokens, input.sampler, matrix);
            if (order == MatrixOrder::None && fragment_matrices.size() == 1U)
                order = wrapped_texture_matrix_order(fragment_tokens);
            if (order != MatrixOrder::None) {
                input.fragment_matrix = matrix;
                input.fragment_matrix_order = order;
                break;
            }
        }
        const auto colors = names(fragment_declarations, "uniform", "vec4");
        if (colors.size() == 1U)
            input.color_uniform = colors.front();
    }
    return result;
}

} // namespace ilemu
