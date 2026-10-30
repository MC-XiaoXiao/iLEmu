// SPDX-License-Identifier: MPL-2.0
#include "network/apple80211_layout.hpp"

#include "arm/a32_decode.hpp"
#include "arm/t32_decode.hpp"
#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/output.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <set>
#include <tuple>
#include <vector>

namespace ilemu {
namespace {
    namespace wire = darwin::network::apple80211_driver;
    enum class Kind { Unknown, Constant, Record, Stack, Field, Object };
    struct Value {
        Kind kind { Kind::Unknown };
        std::uint32_t value { };
        std::uint32_t width { };
        std::uint32_t length { };
    };
    using Fields = std::map<std::string, Value>;

    // A bounded provenance pass, using the same decoder as the interpreter.
    // It never executes firmware, follows indirect code, or writes guest RAM.
    // Only direct CF construction (typed source -> object -> named key) is
    // accepted. Unrecognized arithmetic loses provenance.
    class ConversionReader {
    public:
        explicit ConversionReader(AddressSpace& memory)
            : memory_ { memory }
        {
        }
        std::array<Value, 16> registers { };
        std::map<std::uint32_t, Value> stack;
        Fields fields;
        std::set<std::uint32_t> number_sources;
        std::map<std::uint32_t, unsigned> record_reads;
        void record_range(std::uint32_t address, std::uint32_t size)
        {
            record_address_ = address;
            record_size_ = size;
            for (auto& value : registers)
                value = normalize(value);
        }
        std::function<void(std::uint32_t, bool)> on_call;
        std::function<void(std::uint32_t, const std::array<Value, 16>&)>
            on_return;

        void read(std::uint32_t address, bool thumb, unsigned limit)
        {
            struct Path {
                std::uint32_t pc;
                unsigned it;
                std::optional<bool> zero;
                std::array<Value, 16> registers;
                std::map<std::uint32_t, Value> stack;
            };
            const auto begin = address;
            std::vector<Path> pending;
            std::set<std::pair<std::uint32_t, std::size_t>> visited;
            std::map<std::uint32_t, unsigned> visits;
            unsigned it = 0;
            for (unsigned count = 0; count < limit; ++count) {
                const auto signature = [&] {
                    std::size_t hash =
                        it * 3U + (zero_ ? (*zero_ ? 1U : 2U) : 0U);
                    const auto mix = [&](const Value& v) {
                        for (const auto n : { static_cast<unsigned>(v.kind),
                                 v.value, v.width, v.length })
                            hash ^= n + 0x9e3779b9U + (hash << 6) + (hash >> 2);
                    };
                    for (unsigned r = 0; r < 15; ++r)
                        mix(registers[r]);
                    for (const auto& [offset, value] : stack) {
                        hash ^= offset;
                        mix(value);
                    }
                    return hash;
                };
                const auto resume = [&] {
                    if (pending.empty())
                        return false;
                    auto path = std::move(pending.back());
                    pending.pop_back();
                    address = path.pc;
                    it = path.it;
                    zero_ = path.zero;
                    registers = path.registers;
                    stack = std::move(path.stack);
                    return true;
                };
                if (address < begin || address - begin >= 8192 ||
                    !visited.emplace(address, signature()).second ||
                    ++visits[address] > 8) {
                    if (!resume())
                        return;
                    continue;
                }
                const auto first = memory_.read16(address);
                const auto second = memory_.read16(address + 2);
                if (!first ||
                    ((!thumb || arm::thumb_is_wide(*first)) && !second)) {
                    if (!resume())
                        return;
                    continue;
                }
                const auto i =
                    thumb ? arm::decode_t32(*first, second, it)
                          : arm::decode_a32(
                                *first | (std::uint32_t { *second } << 16));
                using I = arm::InstructionKind;
                const auto next = address + i.size;
                const auto next_it = i.kind == I::IfThen ? i.immediate
                                     : it == 0           ? 0
                                                         : arm::advance_it(it);
                registers[15] = { Kind::Constant,
                    (address + i.pc_offset) & (i.align_pc ? ~3U : ~0U) };
                const auto condition =
                    zero_ && (i.condition == 0 || i.condition == 1)
                        ? std::optional<bool> { i.condition == 0 ? *zero_
                                                                 : !*zero_ }
                        : std::nullopt;
                if (condition == false) {
                    address = next;
                    it = next_it;
                    continue;
                }
                if ((i.kind == I::Branch && !i.link) ||
                    i.kind == I::CompareBranch) {
                    const auto target = registers[15].value + i.immediate;
                    if ((i.condition == 14 || condition == true) &&
                        i.kind == I::Branch)
                        address = target;
                    else {
                        pending.push_back(
                            { target, next_it, zero_, registers, stack });
                        address = next;
                    }
                    it = next_it;
                    continue;
                }
                const bool returns =
                    (i.kind == I::BranchExchange && !i.link) ||
                    (i.kind == I::MultipleTransfer && i.load &&
                        (i.registers & 0x8000)) ||
                    (i.kind == I::Transfer && i.load && i.rd == 15);
                if (returns) {
                    visit(i, thumb);
                    const auto destination = i.kind == I::BranchExchange
                                                 ? registers[i.rm]
                                                 : registers[15];
                    if (on_return && destination.kind == Kind::Constant)
                        on_return(destination.value, registers);
                }
                if (i.kind == I::Unsupported || returns) {
                    if (!resume())
                        return;
                    continue;
                }
                if (i.condition < 14 && !condition) {
                    // Visit the path retaining current provenance first. Error
                    // arms often clear the BSS pointer in predicated moves.
                    const auto before = registers;
                    const auto before_stack = stack;
                    const auto before_zero = zero_;
                    visit(i, thumb);
                    pending.push_back(
                        { next, next_it, zero_, registers, stack });
                    registers = before;
                    stack = before_stack;
                    zero_ = before_zero;
                } else {
                    visit(i, thumb);
                }
                address = next;
                it = next_it;
            }
        }

    private:
        std::optional<bool> is_zero(Value value) const
        {
            if (value.kind == Kind::Constant)
                return value.value == 0;
            if (value.kind == Kind::Record || value.kind == Kind::Object ||
                value.kind == Kind::Stack)
                return false;
            return std::nullopt;
        }
        Value normalize(Value value) const
        {
            if (record_size_ != 0 && value.kind == Kind::Constant &&
                value.value >= record_address_ &&
                value.value - record_address_ < record_size_)
                return { Kind::Record, value.value - record_address_ };
            return value;
        }
        Value add(Value base, std::uint32_t offset) const
        {
            if (offset == 0)
                return base;
            if (base.kind == Kind::Constant || base.kind == Kind::Record ||
                base.kind == Kind::Stack) {
                base.value += offset;
                return normalize(base);
            }
            return { };
        }
        Value load(Value address, unsigned width)
        {
            address = normalize(address);
            if (address.kind == Kind::Record) {
                record_reads.emplace(address.value, width);
                return { Kind::Field, address.value, width };
            }
            if (address.kind == Kind::Stack) {
                const auto found = stack.find(address.value);
                return found == stack.end() ? Value { } : found->second;
            }
            if (address.kind != Kind::Constant)
                return { };
            std::optional<std::uint32_t> value;
            if (width == 4)
                value = memory_.read32(address.value);
            else if (width == 2)
                value = memory_.read16(address.value);
            else if (width == 1)
                value = memory_.read8(address.value);
            return value ? Value { Kind::Constant, *value } : Value { };
        }
        std::optional<std::string> key(Value value)
        {
            if (value.kind != Kind::Constant)
                return std::nullopt;
            // Constant CFString's ARM32 bytes/length words. No ObjC runtime.
            const auto pointer = memory_.read32(value.value + 8);
            const auto length = memory_.read32(value.value + 12);
            if (!pointer || !length || *length == 0 || *length > 64)
                return std::nullopt;
            const auto text = memory_.read_c_string(*pointer, *length + 1);
            if (!text || text->size() != *length)
                return std::nullopt;
            return text;
        }
        void call(std::uint32_t target, bool thumb)
        {
            if (on_call)
                on_call(target, thumb);
            const auto input = registers;
            Value result;
            if (const auto name = key(input[1]);
                name && input[2].kind == Kind::Object) {
                fields.insert_or_assign(*name, input[2]);
            } else if (input[1].kind == Kind::Constant &&
                       (input[1].value == 2 || input[1].value == 3) &&
                       (input[2].kind == Kind::Record ||
                           (input[2].kind == Kind::Stack &&
                               load(input[2], input[1].value == 2 ? 2U : 4U)
                                       .kind == Kind::Field))) {
                const auto source =
                    input[2].kind == Kind::Record
                        ? input[2]
                        : load(input[2], input[1].value == 2 ? 2U : 4U);
                result = { Kind::Object, source.value,
                    input[1].value == 2 ? 2U : 4U };
                if (input[1].value == 3)
                    number_sources.insert(source.value);
            } else if (input[1].kind == Kind::Record &&
                       input[2].kind == Kind::Field) {
                result = { Kind::Object, input[1].value, 0, input[2].value };
                // Native CFStringCreateWithBytes: UTF-8 bytes immediately
                // follow the byte-sized SSID length in the BSS record.
                if (input[2].width == 1 &&
                    input[1].value == input[2].value + 1 &&
                    input[3].kind == Kind::Constant &&
                    input[3].value == 0x08000100)
                    fields.try_emplace("SSID_STR", result);
            } else if (input[1].kind == Kind::Object) {
                result = input[1]; // string made from the native BSSID bytes
            } else if (input[0].kind == Kind::Record) {
                result = { Kind::Object, input[0].value };
            } else if (input[0].kind == Kind::Object) {
                result = input[0]; // retained CF object
            }
            for (const auto r : { 0, 1, 2, 3, 12 })
                registers[r] = { };
            registers[0] = result;
            zero_.reset();
        }
        void visit(const arm::Instruction& i, bool thumb)
        {
            using I = arm::InstructionKind;
            if (i.kind == I::DataProcessing) {
                if (i.opcode >= 8 && i.opcode <= 11) {
                    zero_.reset();
                    if (i.opcode == 10 && i.immediate_operand &&
                        i.immediate == 0)
                        zero_ = is_zero(registers[i.rn]);
                    return; // comparisons have no destination
                }
                auto right = i.immediate_operand
                                 ? Value { Kind::Constant, i.immediate }
                                 : registers[i.rm];
                if (!i.immediate_operand &&
                    (i.register_shift || i.shift != arm::ShiftKind::Lsl ||
                        i.shift_amount != 0))
                    right = { };
                Value result;
                if (i.opcode == 13)
                    result = right;
                else if (right.kind == Kind::Constant &&
                         (i.opcode == 4 || i.opcode == 2))
                    result = add(registers[i.rn],
                        i.opcode == 4 ? right.value : 0U - right.value);
                else if (i.opcode == 15 && right.kind == Kind::Constant)
                    result = { Kind::Constant, ~right.value };
                registers[i.rd] = result;
                if (i.set_flags)
                    zero_ = is_zero(result);
            } else if (i.kind == I::WideImmediate) {
                registers[i.rd] = i.opcode == 0
                                      ? Value { Kind::Constant, i.immediate }
                                  : registers[i.rd].kind == Kind::Constant
                                      ? Value { Kind::Constant,
                                            (registers[i.rd].value & 0xffff) |
                                                (i.immediate << 16) }
                                      : Value { };
            } else if (i.kind == I::Transfer) {
                const auto offset = i.immediate_operand
                                        ? Value { Kind::Constant, i.immediate }
                                        : registers[i.rm];
                const auto base = registers[i.rn];
                const auto adjusted =
                    offset.kind == Kind::Constant
                        ? add(base, i.add ? offset.value : 0U - offset.value)
                        : Value { };
                const auto address = i.index ? adjusted : base;
                if (i.load)
                    registers[i.rd] = load(address, i.access_size);
                else if (address.kind == Kind::Stack)
                    stack[address.value] = registers[i.rd];
                if (i.writeback)
                    registers[i.rn] = adjusted;
            } else if (i.kind == I::MultipleTransfer) {
                const auto base = registers[i.rn];
                const auto bytes =
                    4U * static_cast<unsigned>(std::popcount(i.registers));
                auto location =
                    add(base, i.add ? (i.index ? 4U : 0U)
                                    : 0U - bytes + (i.index ? 0U : 4U));
                for (unsigned r = 0; r < 16; ++r) {
                    if ((i.registers & (1U << r)) == 0)
                        continue;
                    if (i.load)
                        registers[r] = load(location, 4);
                    else if (location.kind == Kind::Stack)
                        stack[location.value] = registers[r];
                    location = add(location, 4);
                }
                if (i.writeback)
                    registers[i.rn] = add(base, i.add ? bytes : 0U - bytes);
            } else if (i.kind == I::Branch && i.link) {
                auto target = registers[15].value + i.immediate;
                call(target & ~1U, thumb != i.exchange);
            } else if (i.kind == I::BranchExchange && i.link &&
                       registers[i.rm].kind == Kind::Constant) {
                const auto target = registers[i.rm].value;
                call(target & ~1U, (target & 1U) != 0);
            }
        }
        AddressSpace& memory_;
        std::uint32_t record_address_ { };
        std::uint32_t record_size_ { };
        std::optional<bool> zero_;
    };

    using Registers = std::array<Value, 16>;
    struct Consumer {
        std::uint32_t pc;
        Registers registers;
    };
    std::vector<Consumer> consumers(AddressSpace& memory, Cpu& cpu)
    {
        Registers registers;
        for (unsigned r = 0; r < 16; ++r)
            registers[r] = { Kind::Constant, cpu.registers()[r] };
        registers[0] = { Kind::Constant,
            0 }; // analyze the successful ioctl return
        std::vector<Consumer> result { { cpu.registers()[14], registers } };
        // Trace only a bounded syscall veneer epilogue to restore the saved
        // registers/SP. Cancellation-aware libc ioctl has an extra frame.
        std::set<std::uint32_t> visited { result.front().pc };
        for (std::size_t index = 0; index < result.size() && index < 3;
            ++index) {
            ConversionReader veneer { memory };
            veneer.registers = result[index].registers;
            veneer.on_return = [&](std::uint32_t pc,
                                   const Registers& restored) {
                if (result.size() < 8 && visited.insert(pc).second)
                    result.push_back({ pc, restored });
            };
            veneer.read(
                result[index].pc & ~1U, (result[index].pc & 1U) != 0, 120);
        }
        return result;
    }
    std::optional<Fields> network_fields(AddressSpace& memory,
        const Consumer& consumer, std::uint32_t data_address)
    {
        std::set<std::uint32_t> visited;
        std::optional<Fields> result;
        ConversionReader caller { memory };
        caller.registers = consumer.registers;
        caller.on_call = [&](std::uint32_t target, bool thumb) {
            const auto argument = std::find_if(caller.registers.begin(),
                caller.registers.begin() + 4,
                [data_address](const Value& value) {
                    return value.kind == Kind::Constant &&
                           value.value == data_address;
                });
            if (result || argument == caller.registers.begin() + 4 ||
                !visited.insert(target).second)
                return;
            ConversionReader converter { memory };
            converter.registers[static_cast<std::size_t>(
                argument - caller.registers.begin())] = { Kind::Record, 0 };
            converter.registers[13] = { Kind::Stack, 0x10000 };
            converter.read(target, thumb, 12000);
            if (converter.fields.contains("RSSI") &&
                converter.fields.contains("CHANNEL") &&
                converter.fields.contains("SSID_STR") &&
                converter.fields.contains("BSSID")) {
                const auto end = converter.fields.at("SSID_STR").value + 32;
                for (const auto& [offset, width] : converter.record_reads) {
                    if (offset >= end && width == 2)
                        converter.fields["@IE_LENGTH"] = { Kind::Field, offset,
                            width };
                    if (offset >= end && width == 4)
                        converter.fields["@IE_POINTER"] = { Kind::Field, offset,
                            width };
                }
                result = std::move(converter.fields);
            }
        };
        caller.read(consumer.pc & ~1U, (consumer.pc & 1U) != 0, 2000);
        return result;
    }

} // namespace

void Apple80211LayoutResolver::reset()
{
    std::lock_guard lock { mutex_ };
    networks_.clear();
    signals_.clear();
}

std::optional<wire::NetworkRecordLayout>
Apple80211LayoutResolver::network_record(AddressSpace& memory, Cpu& cpu,
    Output& output, std::uint32_t address, std::uint32_t size)
{
    if (address == 0 || size < 32 || size > 4096 || address > UINT32_MAX - size)
        return std::nullopt;
    std::lock_guard lock { mutex_ };
    std::pair<std::uint32_t, std::uint32_t> identity { cpu.registers()[14],
        size };
    std::optional<Fields> fields;
    const auto callers = consumers(memory, cpu);
    for (const auto& consumer : callers) {
        const auto cached = networks_.find({ consumer.pc, size });
        if (cached != networks_.end())
            return cached->second;
    }
    for (const auto& consumer : callers) {
        identity.first = consumer.pc;
        if (const auto cached = networks_.find(identity);
            cached != networks_.end())
            return cached->second;
        fields = network_fields(memory, consumer, address);
        if (fields)
            break;
    }
    if (!fields) {
        output.write("[wifi-layout] unresolved consumer=" +
                     std::to_string(identity.first) +
                     " size=" + std::to_string(size) + "\n");
        return std::nullopt;
    }
    wire::NetworkRecordLayout layout { size };
    // Prefer the native parser's tail accesses. An initialized caller-owned
    // capacity also distinguishes packed tails in rounded request buffers.
    const auto native_length = fields->find("@IE_LENGTH");
    const auto native_pointer = fields->find("@IE_POINTER");
    if (native_length != fields->end() && native_pointer != fields->end() &&
        native_length->second.value <= size - 2 &&
        native_pointer->second.value <= size - 4) {
        layout.ie_length_offset = native_length->second.value;
        layout.ie_pointer_offset = native_pointer->second.value;
    } else {
        layout.ie_pointer_offset = size - 4;
        for (const auto offset : { size - 8, size - 6 }) {
            const auto length = memory.read16(address + offset);
            const auto pointer =
                memory.read32(address + layout.ie_pointer_offset);
            if (length && *length != 0 && pointer && *pointer != 0 &&
                memory.read_bytes(*pointer, *length)) {
                if (layout.ie_length_offset != 0)
                    return std::nullopt;
                layout.ie_length_offset = offset;
            }
        }
    }
    if (layout.ie_length_offset == 0)
        return std::nullopt;
    const auto capacity = memory.read16(address + layout.ie_length_offset);
    const auto scratch = memory.read32(address + layout.ie_pointer_offset);
    if (!capacity || !scratch ||
        (*capacity != 0 &&
            (*scratch == 0 || !memory.read_bytes(*scratch, *capacity))))
        return std::nullopt;
    const auto field = [&](std::string_view name, std::uint32_t width,
                           std::uint32_t& offset) {
        const auto found = fields->find(std::string { name });
        if (found == fields->end() || found->second.width != width ||
            found->second.value > size - width)
            return false;
        offset = found->second.value;
        return true;
    };
    if (!field("CHANNEL", 4, layout.channel_offset) ||
        !field("RSSI", 2, layout.rssi_offset) ||
        !field("NOISE", 2, layout.noise_offset) ||
        !field("BEACON_INT", 2, layout.beacon_interval_offset) ||
        !field("CAPABILITIES", 2, layout.capabilities_offset) ||
        !field("CHANNEL_FLAGS", 4, layout.channel_flags_offset))
        return std::nullopt;
    const auto& ssid = fields->at("SSID_STR");
    const auto& bssid = fields->at("BSSID");
    if (ssid.width != 0 || ssid.value > size - 32 || ssid.length >= size ||
        bssid.width != 0 || bssid.value > size - 6)
        return std::nullopt;
    layout.ssid_offset = ssid.value;
    layout.ssid_length_offset = ssid.length;
    layout.bssid_offset = bssid.value;
    if (!wire::make_network_record(nullptr, layout, 0, 0))
        return std::nullopt;
    output.write(
        "[wifi-layout] discovered consumer=" + std::to_string(identity.first) +
        " size=" + std::to_string(size) +
        " rssi=" + std::to_string(layout.rssi_offset) +
        " ssid=" + std::to_string(layout.ssid_offset) +
        " ie=" + std::to_string(layout.ie_length_offset) + "/" +
        std::to_string(layout.ie_pointer_offset) + "\n");
    networks_.emplace(identity, layout);
    return layout;
}
} // namespace ilemu

namespace ilemu {
bool Apple80211LayoutResolver::write_signal(AddressSpace& memory, Cpu& cpu,
    Output& output, std::uint32_t address, std::uint32_t size,
    std::int32_t signal)
{
    if (address == 0 || size < 16 || size > 4096 || address > UINT32_MAX - size)
        return false;
    std::lock_guard lock { mutex_ };
    std::optional<SignalLayout> layout;
    std::uint32_t consumer_pc = 0;
    const auto callers = consumers(memory, cpu);
    for (const auto& consumer : callers) {
        if (const auto found = signals_.find({ consumer.pc, size });
            found != signals_.end()) {
            layout = found->second;
            break;
        }
    }
    for (const auto& consumer : callers) {
        if (layout)
            break;
        const auto identity = std::pair { consumer.pc, size };
        ConversionReader reader { memory };
        reader.registers = consumer.registers;
        reader.record_range(address, size);
        reader.read(consumer.pc & ~1U, (consumer.pc & 1U) != 0, 5000);
        SignalLayout discovered { };
        std::optional<std::uint32_t> unit;
        for (const auto& [name, field] : reader.fields) {
            if (field.width != 4 || field.value > size - 4 ||
                (!name.starts_with("RSSI") && !name.starts_with("NOISE")))
                continue;
            if (name.ends_with("UNIT"))
                unit = field.value;
            else
                discovered.values.push_back(field.value);
        }
        if (!unit || discovered.values.empty())
            continue;
        // Native signal arrays carry a count before their unit and samples.
        // Recover that header read independently from the value offsets.
        const auto count = std::find_if(reader.record_reads.begin(),
            reader.record_reads.end(), [unit](const auto& field) {
                return field.first != 0 && field.first < *unit &&
                       field.second == 4;
            });
        if (count == reader.record_reads.end())
            continue;
        discovered.count = count->first;
        discovered.unit = *unit;
        // Scalar and array forms can use the same dictionary key. Retain all
        // typed record sources, including antenna samples and aggregates.
        for (const auto offset : reader.number_sources) {
            if (offset > *unit && offset <= size - 4)
                discovered.values.push_back(offset);
        }
        std::sort(discovered.values.begin(), discovered.values.end());
        std::erase(discovered.values, discovered.count);
        std::erase(discovered.values, discovered.unit);
        discovered.values.erase(
            std::unique(discovered.values.begin(), discovered.values.end()),
            discovered.values.end());
        if (discovered.values.empty())
            continue;
        consumer_pc = consumer.pc;
        layout = discovered;
        signals_.emplace(identity, std::move(discovered));
        output.write(
            "[wifi-layout] signal consumer=" + std::to_string(consumer_pc) +
            " size=" + std::to_string(size) +
            " count=" + std::to_string(layout->count) +
            " unit=" + std::to_string(layout->unit) +
            " values=" + std::to_string(layout->values.size()) + "\n");
        break;
    }
    if (!layout)
        return false;
    // Preserve firmware version/reserved words, and publish only fields whose
    // native consumers were identified, including scalar and antenna forms.
    if (!memory.write32(address + layout->count, 1) ||
        !memory.write32(address + layout->unit, 0))
        return false;
    for (const auto offset : layout->values) {
        if (!memory.write32(
                address + offset, static_cast<std::uint32_t>(signal)))
            return false;
    }
    return true;
}
} // namespace ilemu
