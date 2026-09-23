// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "guest_drawable_geometry.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <utility>

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/userland_hle.hpp"

namespace ilemu {
namespace {
    constexpr std::uint32_t utf8_encoding = 0x08000100U;
    constexpr std::uint32_t maximum_drawable_extent = 8192U;
}

GuestDrawableGeometryReader::GuestDrawableGeometryReader(
    std::uint32_t drawable, Completion completion)
    : drawable_ { drawable }
    , completion_ { std::move(completion) }
{
}

void GuestDrawableGeometryReader::read(UserlandHleCall& call,
    std::uint32_t drawable, Completion completion)
{
    auto reader = std::shared_ptr<GuestDrawableGeometryReader>(
        new GuestDrawableGeometryReader(drawable, std::move(completion)));
    reader->read_bounds(call);
}

void GuestDrawableGeometryReader::complete(UserlandHleCall& call,
    std::optional<GuestDrawableGeometry> geometry)
{
    if (completion_)
        std::exchange(completion_, { })(call, geometry);
}

void GuestDrawableGeometryReader::message(UserlandHleCall& call,
    std::uint32_t object, std::string_view selector,
    std::uint32_t argument, ValueCompletion completion)
{
    const auto name = call.intern_string(selector);
    if (!object || !name) {
        completion(call, 0U);
        return;
    }
    call.cpu().registers()[0] = name;
    if (!call.call_guest_function("_sel_registerName",
            [object, argument, completion](UserlandHleCall& selected) {
                const auto sel = selected.cpu().registers()[0];
                if (!sel) {
                    completion(selected, 0U);
                    return;
                }
                auto& registers = selected.cpu().registers();
                registers[0] = object;
                registers[1] = sel;
                registers[2] = argument;
                if (!selected.call_guest_function("_objc_msgSend",
                        [completion](UserlandHleCall& returned) {
                            completion(returned, returned.cpu().registers()[0]);
                        })) {
                    completion(selected, 0U);
                }
            })) {
        completion(call, 0U);
    }
}

void GuestDrawableGeometryReader::property(UserlandHleCall& call,
    std::string_view name, ValueCompletion completion)
{
    const auto key_name = call.intern_string(name);
    if (!key_name) {
        completion(call, 0U);
        return;
    }
    auto& registers = call.cpu().registers();
    registers[0] = 0U;
    registers[1] = key_name;
    registers[2] = utf8_encoding;
    auto self = shared_from_this();
    if (!call.call_guest_function("_CFStringCreateWithCString",
            [self, completion](UserlandHleCall& created) {
                const auto key = created.cpu().registers()[0];
                if (!key) {
                    completion(created, 0U);
                    return;
                }
                self->message(created, self->drawable_, "valueForKey:", key,
                    [key, completion](UserlandHleCall& returned,
                        std::uint32_t value) {
                        returned.cpu().registers()[0] = key;
                        if (!returned.call_guest_function("_CFRelease",
                                [completion, value](UserlandHleCall& released) {
                                    completion(released, value);
                                })) {
                            completion(returned, value);
                        }
                    });
            })) {
        completion(call, 0U);
    }
}

void GuestDrawableGeometryReader::read_bounds(UserlandHleCall& call)
{
    auto self = shared_from_this();
    property(call, "bounds", [self](UserlandHleCall& boxed,
                                std::uint32_t value) {
        const auto output = boxed.allocate_data(16U, 4U);
        if (!value || !output) {
            self->complete(boxed, std::nullopt);
            return;
        }
        self->message(boxed, value, "getValue:", output,
            [self, output](UserlandHleCall& read, std::uint32_t) {
                const auto width = read.memory().read32(output + 8U);
                const auto height = read.memory().read32(output + 12U);
                if (!width || !height) {
                    self->complete(read, std::nullopt);
                    return;
                }
                const auto width_value = std::bit_cast<float>(*width);
                const auto height_value = std::bit_cast<float>(*height);
                if (!std::isfinite(width_value) ||
                    !std::isfinite(height_value) || width_value < 1.0F ||
                    height_value < 1.0F ||
                    width_value > maximum_drawable_extent ||
                    height_value > maximum_drawable_extent) {
                    self->complete(read, std::nullopt);
                    return;
                }
                self->bounds_ = {
                    static_cast<std::uint32_t>(std::lround(width_value)),
                    static_cast<std::uint32_t>(std::lround(height_value))
                };
                self->read_scale(read);
            });
    });
}

void GuestDrawableGeometryReader::read_scale(UserlandHleCall& call)
{
    auto self = shared_from_this();
    property(call, "contentsScale", [self](UserlandHleCall& boxed,
                                       std::uint32_t value) {
        if (!value) {
            self->read_window(boxed, 1U);
            return;
        }
        self->message(boxed, value, "integerValue", 0U,
            [self](UserlandHleCall& read, std::uint32_t scale) {
                if (scale == 0U || scale > 4U ||
                    self->bounds_.width > maximum_drawable_extent / scale ||
                    self->bounds_.height > maximum_drawable_extent / scale) {
                    self->complete(read, std::nullopt);
                    return;
                }
                self->read_window(read, scale);
            });
    });
}

void GuestDrawableGeometryReader::complete_with_window(
    UserlandHleCall& call, std::uint32_t scale, std::uint32_t window)
{
    complete(call, GuestDrawableGeometry {
        bounds_,
        { bounds_.width * scale, bounds_.height * scale },
        window
    });
}

void GuestDrawableGeometryReader::read_window(
    UserlandHleCall& call, std::uint32_t scale)
{
    const auto name = call.intern_string("nativeWindow");
    if (!name) {
        complete_with_window(call, scale, 0U);
        return;
    }
    auto self = shared_from_this();
    call.cpu().registers()[0] = name;
    if (!call.call_guest_function("_sel_registerName",
            [self, scale](UserlandHleCall& selected) {
                const auto selector = selected.cpu().registers()[0];
                if (!selector) {
                    self->complete_with_window(selected, scale, 0U);
                    return;
                }
                self->message(selected, self->drawable_,
                    "respondsToSelector:", selector,
                    [self, selector, scale](UserlandHleCall& tested,
                        std::uint32_t responds) {
                        if (!responds) {
                            self->complete_with_window(tested, scale, 0U);
                            return;
                        }
                        tested.cpu().registers()[0] = self->drawable_;
                        tested.cpu().registers()[1] = selector;
                        if (!tested.call_guest_function("_objc_msgSend",
                                [self, scale](UserlandHleCall& returned) {
                                    self->complete_with_window(returned, scale,
                                        returned.cpu().registers()[0]);
                                })) {
                            self->complete_with_window(tested, scale, 0U);
                        }
                    });
            })) {
        complete_with_window(call, scale, 0U);
    }
}

} // namespace ilemu
