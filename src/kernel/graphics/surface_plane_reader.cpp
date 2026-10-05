// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "surface_plane_reader.hpp"

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/userland_hle.hpp"

#include <array>
#include <string>
#include <utility>

namespace ilemu {
namespace {
    struct Property {
        const char* symbol;
        std::uint32_t SurfacePlane::* field;
    };
    constexpr std::array properties {
        Property { "_kIOSurfacePlaneOffset", &SurfacePlane::offset },
        Property { "_kIOSurfacePlaneWidth", &SurfacePlane::width },
        Property { "_kIOSurfacePlaneHeight", &SurfacePlane::height },
        Property {
            "_kIOSurfacePlaneBytesPerRow", &SurfacePlane::bytes_per_row },
        Property { "_kIOSurfacePlaneSize", &SurfacePlane::size },
        Property { "_kIOSurfacePlaneBytesPerElement",
            &SurfacePlane::bytes_per_element },
        Property {
            "_kIOSurfacePlaneElementWidth", &SurfacePlane::element_width },
        Property {
            "_kIOSurfacePlaneElementHeight", &SurfacePlane::element_height },
    };
    std::uint32_t key(UserlandHleCall& call, const char* symbol)
    {
        const auto address = call.symbol_address(symbol);
        return address ? call.memory().read32(*address).value_or(0) : 0;
    }
}

void SurfacePlaneReader::register_symbols(UserlandHleRegistry& registry)
{
    for (const auto& property : properties)
        registry.register_guest_data_symbol(
            "/IOSurface.framework/IOSurface", property.symbol);
    registry.register_guest_data_symbol(
        "/IOSurface.framework/IOSurface", "_kIOSurfacePlaneInfo");
    for (const auto* symbol : { "_CFArrayGetCount", "_CFArrayGetValueAtIndex" })
        registry.register_guest_function(
            "/CoreFoundation.framework/CoreFoundation", symbol);
}

void SurfacePlaneReader::read(UserlandHleCall& call, std::uint32_t dictionary,
    std::uint32_t number_output, Completion completion)
{
    const auto plane_key = key(call, "_kIOSurfacePlaneInfo");
    if (!plane_key) {
        completion(call, { });
        return;
    }
    auto reader = std::make_shared<SurfacePlaneReader>();
    reader->number_output_ = number_output;
    reader->completion_ = std::move(completion);
    call.cpu().registers()[0] = dictionary;
    call.cpu().registers()[1] = plane_key;
    if (!call.call_guest_function(
            "_CFDictionaryGetValue", [reader](UserlandHleCall& value) {
                reader->array_ = value.argument(0);
                if (!reader->array_) {
                    reader->completion_(value, { });
                    return;
                }
                value.cpu().registers()[0] = reader->array_;
                if (!value.call_guest_function(
                        "_CFArrayGetCount", [reader](UserlandHleCall& count) {
                            reader->count_ = count.argument(0);
                            // Bound memory use and reject malformed signed
                            // CFIndex values.
                            if (reader->count_ > 16) {
                                count.set_return(0);
                                return;
                            }
                            reader->next_plane(count);
                        }))
                    value.set_return(0);
            }))
        call.set_return(0);
}

void SurfacePlaneReader::next_plane(UserlandHleCall& call)
{
    if (planes_.size() == count_) {
        completion_(call, std::move(planes_));
        return;
    }
    call.cpu().registers()[0] = array_;
    call.cpu().registers()[1] = static_cast<std::uint32_t>(planes_.size());
    if (!call.call_guest_function("_CFArrayGetValueAtIndex",
            [self = shared_from_this()](UserlandHleCall& value) {
                self->dictionary_ = value.argument(0);
                if (!self->dictionary_) {
                    value.set_return(0);
                    return;
                }
                self->planes_.emplace_back();
                self->property_ = 0;
                self->next_property(value);
            }))
        call.set_return(0);
}

void SurfacePlaneReader::next_property(UserlandHleCall& call)
{
    if (property_ == properties.size()) {
        next_plane(call);
        return;
    }
    const auto property_key = key(call, properties[property_].symbol);
    if (!property_key) {
        ++property_;
        next_property(call);
        return;
    }
    call.cpu().registers()[0] = dictionary_;
    call.cpu().registers()[1] = property_key;
    if (!call.call_guest_function("_CFDictionaryGetValue",
            [self = shared_from_this()](UserlandHleCall& value) {
                if (!value.argument(0)) {
                    ++self->property_;
                    self->next_property(value);
                    return;
                }
                value.cpu().registers()[1] = 3; // kCFNumberSInt32Type
                value.cpu().registers()[2] = self->number_output_;
                if (!value.call_guest_function(
                        "_CFNumberGetValue", [self](UserlandHleCall& number) {
                            if (!number.argument(0)) {
                                number.set_return(0);
                                return;
                            }
                            self->planes_.back().*
                                properties[self->property_].field =
                                number.memory()
                                    .read32(self->number_output_)
                                    .value_or(0);
                            ++self->property_;
                            self->next_property(number);
                        }))
                    value.set_return(0);
            }))
        call.set_return(0);
}
} // namespace ilemu
