// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "graphics/gles_filter_profile.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <optional>
#include <regex>

namespace ilemu {
namespace {
    std::optional<std::size_t> array_index(const std::string& text)
    {
        std::size_t value { };
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc { } ||
            parsed.ptr != text.data() + text.size() || value >= 1024U)
            return std::nullopt;
        return value;
    }

    std::string compact(std::string_view source)
    {
        auto text = std::regex_replace(
            std::string(source), std::regex(R"(/\*[\s\S]*?\*/|//[^\n]*)"), "");
        text = std::regex_replace(text,
            std::regex(
                R"(\b(?:(?:lowp|mediump|highp)\s+)?(?:vec[234]|float)\s+)"),
            "");
        std::erase_if(text, [](unsigned char c) { return std::isspace(c); });
        return text;
    }
}

GlesFilterProfile GlesFilterProfile::from_sources(
    std::string_view vertex_source, std::string_view fragment_source)
{
    GlesFilterProfile result;
    const auto vertex = compact(vertex_source);
    const auto fragment = compact(fragment_source);
    const std::regex sample_pattern(R"((\w+)=texture2D\((\w+),(\w+)\);)");
    std::map<std::string, std::string> samples;
    for (auto it = std::sregex_iterator(
             fragment.begin(), fragment.end(), sample_pattern);
        it != std::sregex_iterator(); ++it) {
        const auto& match = *it;
        if (!result.sampler.empty() && result.sampler != match[2].str())
            return { };
        result.sampler = match[2].str();
        samples.emplace(match[1].str(), match[3].str());
    }
    if (samples.empty())
        return { };

    if (samples.size() == 1U) {
        const auto& sampled = samples.begin()->first;
        // vec3[4] columns transform RGB and add alpha-scaled bias.
        const std::regex matrix_pattern(
            sampled + R"(\.r\*(\w+)\[0\]\+)" + sampled + R"(\.g\*\1\[1\]\+)" +
            sampled + R"(\.b\*\1\[2\]\+)" + sampled + R"(\.a\*\1\[3\],)" +
            sampled + R"(\.a\))");
        std::smatch match;
        if (std::regex_search(fragment, match, matrix_pattern)) {
            result.operation = Operation::ColorMatrix;
            result.color_matrix = match[1].str();
            return result;
        }
        // A squared-luminance alpha channel used by backdrop statistics.
        const std::regex luminance_pattern(
            R"((\w+)=dot\((\w+)\.rgb,vec3\(\.2125,\.7154,\.0721\)\);returnvec4\(\2\.rgb,\1\*\1\);)");
        if (std::regex_search(fragment, match, luminance_pattern)) {
            result.operation = Operation::LuminanceAlpha;
            return result;
        }
        return { };
    }
    if (samples.size() > maximum_taps)
        return { };

    // Each varying is the same affine base coordinate plus a uniform offset.
    const std::regex offset_pattern(
        R"((\w+)=(\w+)([+-])(\w+)\[([0-9]+)\]\*(\w+)\.xy;)");
    struct Offset {
        Tap tap;
        std::string base;
        std::string transform;
        std::string uniform;
    };
    std::map<std::string, Offset> offsets;
    for (auto it =
             std::sregex_iterator(vertex.begin(), vertex.end(), offset_pattern);
        it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        const auto index = array_index(m[5].str());
        if (!index)
            return { };
        offsets.emplace(m[1].str(),
            Offset { { *index, m[3].str() == "-" ? -1.0F : 1.0F, 0U },
                m[2].str(), m[6].str(), m[4].str() });
    }
    // Accept a linear accumulation whose final value is the fragment output.
    // Every sampled varying must contribute exactly once.
    const std::regex sum_pattern(
        R"((\w+)=\(?(\w+)(?:\+(\w+))?\)?\*(\w+)(?:\[([0-9]+)\])?\.([xyzw])(?:\+(\w+))?;)");
    std::map<std::string, std::size_t> sample_weights;
    std::string accumulator;
    for (auto it = std::sregex_iterator(
             fragment.begin(), fragment.end(), sum_pattern);
        it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        if (accumulator.empty()) {
            if (m[7].matched)
                return { };
            accumulator = m[1].str();
        } else if (m[1].str() != accumulator || m[7].str() != accumulator) {
            return { };
        }
        if (!result.weights.empty() && result.weights != m[4].str())
            return { };
        result.weights = m[4].str();
        const auto index = m[5].matched ? array_index(m[5].str())
                                        : std::optional<std::size_t>(0U);
        if (!index)
            return { };
        const auto component =
            *index * 4U + std::string_view("xyzw").find(m[6].str());
        for (const auto group : { 2, 3 }) {
            if (!m[group].matched)
                continue;
            const auto name = m[group].str();
            if (!samples.contains(name) ||
                !sample_weights.emplace(name, component).second)
                return { };
        }
    }
    const std::regex output_pattern(
        R"((gl_FragData\[0\]|gl_FragColor)=)" + accumulator + ";");
    if (accumulator.empty() || sample_weights.size() != samples.size() ||
        !std::regex_search(fragment, output_pattern))
        return { };
    for (const auto& [sample, varying] : samples) {
        const auto offset = offsets.find(varying);
        if (offset == offsets.end())
            return { };
        const auto& input = offset->second;
        const std::regex base_pattern(input.base + R"(=(\w+)\*)" +
                                      input.transform + R"(\.xy\+)" +
                                      input.transform + R"(\.zw;)");
        std::smatch base;
        if (!std::regex_search(vertex, base, base_pattern))
            return { };
        if (!result.offsets.empty() &&
            (result.offsets != input.uniform ||
                result.coordinate_transform != input.transform ||
                result.coordinate_attribute != base[1].str()))
            return { };
        result.offsets = input.uniform;
        result.coordinate_transform = input.transform;
        result.coordinate_attribute = base[1].str();
        auto tap = input.tap;
        tap.weight_index = sample_weights.at(sample);
        result.taps.push_back(tap);
    }
    result.operation = Operation::WeightedSamples;
    return result;
}
} // namespace ilemu
