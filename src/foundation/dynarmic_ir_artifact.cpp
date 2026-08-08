#include "dynarmic_ir_artifact.hpp"

#include <array>
#include <algorithm>
#include <boost/variant/apply_visitor.hpp>
#include <limits>
#include <unordered_map>
#include <utility>

#include <dynarmic/frontend/A32/a32_types.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/frontend/A64/a64_types.h>
#include <dynarmic/ir/acc_type.h>
#include <dynarmic/ir/cond.h>
#include <dynarmic/ir/opcodes.h>
#include <dynarmic/ir/terminal.h>
#include <dynarmic/ir/type.h>

namespace ilemu {
namespace {

constexpr std::array<std::uint8_t, 8> magic{
    'i', 'L', 'I', 'R', 'B', '0', '0', '1'};
constexpr std::uint32_t format_version = 1U;
constexpr std::size_t maximum_bytes = 16U * 1024U * 1024U;
constexpr std::uint32_t maximum_instructions = 16U * 1024U;
constexpr std::uint32_t maximum_terminal_depth = 32U;

enum class ValueTag : std::uint8_t {
  Reference,
  Immediate,
};

class Writer {
public:
  void byte(std::uint8_t value) {
    if (bytes_.size() < maximum_bytes) {
      bytes_.push_back(static_cast<std::byte>(value));
    } else {
      valid_ = false;
    }
  }

  void u16(std::uint16_t value) {
    byte(static_cast<std::uint8_t>(value));
    byte(static_cast<std::uint8_t>(value >> 8U));
  }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
      byte(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
      byte(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void raw(std::span<const std::uint8_t> values) {
    for (const auto value : values) byte(value);
  }

  [[nodiscard]] bool valid() const { return valid_; }
  [[nodiscard]] std::vector<std::byte> take() && { return std::move(bytes_); }

private:
  std::vector<std::byte> bytes_;
  bool valid_{true};
};

class Reader {
public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_{bytes} {}

  [[nodiscard]] bool byte(std::uint8_t& value) {
    if (offset_ >= bytes_.size()) return false;
    value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
    return true;
  }

  [[nodiscard]] bool u16(std::uint16_t& value) {
    std::uint8_t low{};
    std::uint8_t high{};
    if (!byte(low) || !byte(high)) return false;
    value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(low) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U));
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
      std::uint8_t current{};
      if (!byte(current)) return false;
      value |= static_cast<std::uint32_t>(current) << shift;
    }
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
      std::uint8_t current{};
      if (!byte(current)) return false;
      value |= static_cast<std::uint64_t>(current) << shift;
    }
    return true;
  }

  [[nodiscard]] bool raw(std::span<std::uint8_t> values) {
    if (values.size() > bytes_.size() - offset_) return false;
    for (auto& value : values) {
      value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
    }
    return true;
  }

  [[nodiscard]] bool at_end() const { return offset_ == bytes_.size(); }

private:
  std::span<const std::byte> bytes_;
  std::size_t offset_{};
};

[[nodiscard]] bool valid_condition(std::uint8_t value) {
  return value <= static_cast<std::uint8_t>(Dynarmic::IR::Cond::NV);
}

[[nodiscard]] bool valid_acc_type(std::uint8_t value) {
  return value <= static_cast<std::uint8_t>(Dynarmic::IR::AccType::SWAP);
}

[[nodiscard]] bool valid_value_type(Dynarmic::IR::Type type) {
  switch (type) {
  case Dynarmic::IR::Type::U1:
  case Dynarmic::IR::Type::U8:
  case Dynarmic::IR::Type::U16:
  case Dynarmic::IR::Type::U32:
  case Dynarmic::IR::Type::U64:
  case Dynarmic::IR::Type::A32Reg:
  case Dynarmic::IR::Type::A32ExtReg:
  case Dynarmic::IR::Type::A64Reg:
  case Dynarmic::IR::Type::A64Vec:
  case Dynarmic::IR::Type::CoprocInfo:
  case Dynarmic::IR::Type::NZCVFlags:
  case Dynarmic::IR::Type::Cond:
  case Dynarmic::IR::Type::AccType:
    return true;
  case Dynarmic::IR::Type::Void:
  case Dynarmic::IR::Type::Opaque:
  case Dynarmic::IR::Type::U128:
  case Dynarmic::IR::Type::Table:
    return false;
  }
  return false;
}

[[nodiscard]] bool canonical_a32_location(
    Dynarmic::IR::LocationDescriptor location) {
  const Dynarmic::A32::LocationDescriptor decoded{location};
  return static_cast<Dynarmic::IR::LocationDescriptor>(decoded).Value() ==
         location.Value();
}

[[nodiscard]] bool valid_terminal(
    const Dynarmic::IR::Terminal& terminal,
    const Dynarmic::A32::LocationDescriptor& initial_location) {
  struct Visitor : boost::static_visitor<bool> {
    const Dynarmic::A32::LocationDescriptor& initial_location;

    explicit Visitor(const Dynarmic::A32::LocationDescriptor& initial)
        : initial_location{initial} {}

    bool operator()(const Dynarmic::IR::Term::Invalid&) const {
      return false;
    }

    bool operator()(const Dynarmic::IR::Term::Interpret& value) const {
      const Dynarmic::A32::LocationDescriptor next{value.next};
      // Both current backends either assert these invariants or do not emit
      // Interpret terminals at all. Keep malformed portable IR off Emit().
      return canonical_a32_location(value.next) && value.num_instructions == 1U &&
             next.TFlag() == initial_location.TFlag() &&
             next.EFlag() == initial_location.EFlag();
    }

    bool operator()(const Dynarmic::IR::Term::ReturnToDispatch&) const {
      return true;
    }

    bool operator()(const Dynarmic::IR::Term::LinkBlock& value) const {
      return canonical_a32_location(value.next);
    }

    bool operator()(const Dynarmic::IR::Term::LinkBlockFast& value) const {
      return canonical_a32_location(value.next);
    }

    bool operator()(const Dynarmic::IR::Term::PopRSBHint&) const {
      return true;
    }

    bool operator()(const Dynarmic::IR::Term::FastDispatchHint&) const {
      return true;
    }

    bool operator()(const Dynarmic::IR::Term::If& value) const {
      return valid_terminal(value.then_, initial_location) &&
             valid_terminal(value.else_, initial_location);
    }

    bool operator()(const Dynarmic::IR::Term::CheckBit& value) const {
      return valid_terminal(value.then_, initial_location) &&
             valid_terminal(value.else_, initial_location);
    }

    bool operator()(const Dynarmic::IR::Term::CheckHalt& value) const {
      return valid_terminal(value.else_, initial_location);
    }
  } visitor{initial_location};
  return boost::apply_visitor(visitor, terminal);
}

void write_location(Writer& writer, Dynarmic::IR::LocationDescriptor location) {
  writer.u64(location.Value());
}

[[nodiscard]] bool read_location(
    Reader& reader, Dynarmic::IR::LocationDescriptor& location) {
  std::uint64_t value{};
  if (!reader.u64(value)) return false;
  location = Dynarmic::IR::LocationDescriptor{value};
  return true;
}

void write_terminal(Writer& writer, const Dynarmic::IR::Terminal& terminal,
                    std::uint32_t depth);

void write_terminal_location(Writer& writer,
                             Dynarmic::IR::LocationDescriptor location) {
  write_location(writer, location);
}

void write_terminal(Writer& writer, const Dynarmic::IR::Terminal& terminal,
                    std::uint32_t depth) {
  if (depth > maximum_terminal_depth) {
    writer.byte(0xffU);
    return;
  }
  struct Visitor : boost::static_visitor<void> {
    Writer& writer;
    std::uint32_t depth;

    Visitor(Writer& writer_, std::uint32_t depth_)
        : writer{writer_}, depth{depth_} {}

    void operator()(const Dynarmic::IR::Term::Invalid&) const {
      writer.byte(0U);
    }
    void operator()(const Dynarmic::IR::Term::Interpret& value) const {
      writer.byte(1U);
      write_terminal_location(writer, value.next);
      if (value.num_instructions > std::numeric_limits<std::uint64_t>::max()) {
        writer.byte(0xffU);
      } else {
        writer.u64(static_cast<std::uint64_t>(value.num_instructions));
      }
    }
    void operator()(const Dynarmic::IR::Term::ReturnToDispatch&) const {
      writer.byte(2U);
    }
    void operator()(const Dynarmic::IR::Term::LinkBlock& value) const {
      writer.byte(3U);
      write_terminal_location(writer, value.next);
    }
    void operator()(const Dynarmic::IR::Term::LinkBlockFast& value) const {
      writer.byte(4U);
      write_terminal_location(writer, value.next);
    }
    void operator()(const Dynarmic::IR::Term::PopRSBHint&) const {
      writer.byte(5U);
    }
    void operator()(const Dynarmic::IR::Term::FastDispatchHint&) const {
      writer.byte(6U);
    }
    void operator()(const Dynarmic::IR::Term::If& value) const {
      writer.byte(7U);
      writer.byte(static_cast<std::uint8_t>(value.if_));
      write_terminal(writer, value.then_, depth + 1U);
      write_terminal(writer, value.else_, depth + 1U);
    }
    void operator()(const Dynarmic::IR::Term::CheckBit& value) const {
      writer.byte(8U);
      write_terminal(writer, value.then_, depth + 1U);
      write_terminal(writer, value.else_, depth + 1U);
    }
    void operator()(const Dynarmic::IR::Term::CheckHalt& value) const {
      writer.byte(9U);
      write_terminal(writer, value.else_, depth + 1U);
    }
  } visitor{writer, depth};
  boost::apply_visitor(visitor, terminal);
}

[[nodiscard]] std::optional<Dynarmic::IR::Terminal> read_terminal(
    Reader& reader, std::uint32_t depth) {
  if (depth > maximum_terminal_depth) return std::nullopt;
  std::uint8_t tag{};
  if (!reader.byte(tag)) return std::nullopt;
  switch (tag) {
  case 1: {
    Dynarmic::IR::LocationDescriptor next{0};
    std::uint64_t count{};
    if (!read_location(reader, next) || !reader.u64(count) || count == 0) {
      return std::nullopt;
    }
    if (count > std::numeric_limits<std::size_t>::max()) {
      return std::nullopt;
    }
    auto terminal = Dynarmic::IR::Term::Interpret{next};
    terminal.num_instructions = static_cast<std::size_t>(count);
    return Dynarmic::IR::Terminal{terminal};
  }
  case 2:
    return Dynarmic::IR::Terminal{Dynarmic::IR::Term::ReturnToDispatch{}};
  case 3: {
    Dynarmic::IR::LocationDescriptor next{0};
    if (!read_location(reader, next)) return std::nullopt;
    return Dynarmic::IR::Terminal{Dynarmic::IR::Term::LinkBlock{next}};
  }
  case 4: {
    Dynarmic::IR::LocationDescriptor next{0};
    if (!read_location(reader, next)) return std::nullopt;
    return Dynarmic::IR::Terminal{Dynarmic::IR::Term::LinkBlockFast{next}};
  }
  case 5:
    return Dynarmic::IR::Terminal{Dynarmic::IR::Term::PopRSBHint{}};
  case 6:
    return Dynarmic::IR::Terminal{Dynarmic::IR::Term::FastDispatchHint{}};
  case 7: {
    std::uint8_t condition{};
    if (!reader.byte(condition) || !valid_condition(condition)) {
      return std::nullopt;
    }
    auto then_terminal = read_terminal(reader, depth + 1U);
    auto else_terminal = read_terminal(reader, depth + 1U);
    if (!then_terminal || !else_terminal) return std::nullopt;
    return Dynarmic::IR::Terminal{Dynarmic::IR::Term::If{
        static_cast<Dynarmic::IR::Cond>(condition), std::move(*then_terminal),
        std::move(*else_terminal)}};
  }
  case 8: {
    auto then_terminal = read_terminal(reader, depth + 1U);
    auto else_terminal = read_terminal(reader, depth + 1U);
    if (!then_terminal || !else_terminal) return std::nullopt;
    return Dynarmic::IR::Terminal{Dynarmic::IR::Term::CheckBit{
        std::move(*then_terminal), std::move(*else_terminal)}};
  }
  case 9: {
    auto else_terminal = read_terminal(reader, depth + 1U);
    if (!else_terminal) return std::nullopt;
    return Dynarmic::IR::Terminal{
        Dynarmic::IR::Term::CheckHalt{std::move(*else_terminal)}};
  }
  default:
    return std::nullopt;
  }
}

struct EncodedValue {
  ValueTag tag{};
  Dynarmic::IR::Type type{Dynarmic::IR::Type::Void};
  std::uint32_t reference{};
  std::uint64_t scalar{};
  std::array<std::uint8_t, 8> coprocessor{};
};

[[nodiscard]] std::optional<EncodedValue> encode_value(
    const Dynarmic::IR::Value& value,
    const std::unordered_map<const Dynarmic::IR::Inst*, std::uint32_t>& indices) {
  if (!value.IsImmediate() || value.IsIdentity()) {
    const auto* instruction = value.GetInst();
    const auto found = indices.find(instruction);
    if (found == indices.end()) return std::nullopt;
    return EncodedValue{ValueTag::Reference, Dynarmic::IR::Type::Opaque,
                        found->second, 0, {}};
  }

  const auto type = value.GetType();
  if (!valid_value_type(type)) return std::nullopt;
  EncodedValue encoded{ValueTag::Immediate, type};
  switch (type) {
  case Dynarmic::IR::Type::U1:
    encoded.scalar = value.GetU1() ? 1U : 0U;
    break;
  case Dynarmic::IR::Type::U8:
    encoded.scalar = value.GetU8();
    break;
  case Dynarmic::IR::Type::U16:
    encoded.scalar = value.GetU16();
    break;
  case Dynarmic::IR::Type::U32:
    encoded.scalar = value.GetU32();
    break;
  case Dynarmic::IR::Type::U64:
    encoded.scalar = value.GetU64();
    break;
  case Dynarmic::IR::Type::A32Reg:
    encoded.scalar = static_cast<std::uint32_t>(value.GetA32RegRef());
    break;
  case Dynarmic::IR::Type::A32ExtReg:
    encoded.scalar = static_cast<std::uint32_t>(value.GetA32ExtRegRef());
    break;
  case Dynarmic::IR::Type::A64Reg:
    encoded.scalar = static_cast<std::uint32_t>(value.GetA64RegRef());
    break;
  case Dynarmic::IR::Type::A64Vec:
    encoded.scalar = static_cast<std::uint32_t>(value.GetA64VecRef());
    break;
  case Dynarmic::IR::Type::CoprocInfo: {
    const auto info = value.GetCoprocInfo();
    std::copy(info.begin(), info.end(), encoded.coprocessor.begin());
    break;
  }
  case Dynarmic::IR::Type::NZCVFlags:
    break;
  case Dynarmic::IR::Type::Cond:
    encoded.scalar = static_cast<std::uint32_t>(value.GetCond());
    break;
  case Dynarmic::IR::Type::AccType:
    encoded.scalar = static_cast<std::uint32_t>(value.GetAccType());
    break;
  default:
    return std::nullopt;
  }
  return encoded;
}

void write_encoded_value(Writer& writer, const EncodedValue& value) {
  writer.byte(static_cast<std::uint8_t>(value.tag));
  if (value.tag == ValueTag::Reference) {
    writer.u32(value.reference);
    return;
  }
  writer.u16(static_cast<std::uint16_t>(value.type));
  switch (value.type) {
  case Dynarmic::IR::Type::U1:
  case Dynarmic::IR::Type::U8:
    writer.byte(static_cast<std::uint8_t>(value.scalar));
    break;
  case Dynarmic::IR::Type::U16:
    writer.u16(static_cast<std::uint16_t>(value.scalar));
    break;
  case Dynarmic::IR::Type::U32:
  case Dynarmic::IR::Type::A32Reg:
  case Dynarmic::IR::Type::A32ExtReg:
  case Dynarmic::IR::Type::A64Reg:
  case Dynarmic::IR::Type::A64Vec:
    writer.u32(static_cast<std::uint32_t>(value.scalar));
    break;
  case Dynarmic::IR::Type::U64:
    writer.u64(value.scalar);
    break;
  case Dynarmic::IR::Type::CoprocInfo:
    writer.raw(value.coprocessor);
    break;
  case Dynarmic::IR::Type::NZCVFlags:
    break;
  case Dynarmic::IR::Type::Cond:
  case Dynarmic::IR::Type::AccType:
    writer.byte(static_cast<std::uint8_t>(value.scalar));
    break;
  default:
    writer.byte(0xffU);
    break;
  }
}

[[nodiscard]] std::optional<Dynarmic::IR::Value> read_value(
    Reader& reader, const std::vector<Dynarmic::IR::Inst*>& instructions,
    std::size_t current_index) {
  std::uint8_t raw_tag{};
  if (!reader.byte(raw_tag)) return std::nullopt;
  if (raw_tag == static_cast<std::uint8_t>(ValueTag::Reference)) {
    std::uint32_t reference{};
    if (!reader.u32(reference) || reference >= current_index ||
        reference >= instructions.size()) {
      return std::nullopt;
    }
    return Dynarmic::IR::Value{instructions[reference]};
  }
  if (raw_tag != static_cast<std::uint8_t>(ValueTag::Immediate)) {
    return std::nullopt;
  }

  std::uint16_t raw_type{};
  if (!reader.u16(raw_type)) return std::nullopt;
  const auto type = static_cast<Dynarmic::IR::Type>(raw_type);
  if (!valid_value_type(type)) return std::nullopt;
  switch (type) {
  case Dynarmic::IR::Type::U1:
  case Dynarmic::IR::Type::U8: {
    std::uint8_t value{};
    if (!reader.byte(value) || (type == Dynarmic::IR::Type::U1 && value > 1U)) {
      return std::nullopt;
    }
    return type == Dynarmic::IR::Type::U1
               ? Dynarmic::IR::Value{value != 0}
               : Dynarmic::IR::Value{value};
  }
  case Dynarmic::IR::Type::U16: {
    std::uint16_t value{};
    if (!reader.u16(value)) return std::nullopt;
    return Dynarmic::IR::Value{value};
  }
  case Dynarmic::IR::Type::U32: {
    std::uint32_t value{};
    if (!reader.u32(value)) return std::nullopt;
    return Dynarmic::IR::Value{value};
  }
  case Dynarmic::IR::Type::U64: {
    std::uint64_t value{};
    if (!reader.u64(value)) return std::nullopt;
    return Dynarmic::IR::Value{value};
  }
  case Dynarmic::IR::Type::A32Reg:
  case Dynarmic::IR::Type::A32ExtReg:
  case Dynarmic::IR::Type::A64Reg:
  case Dynarmic::IR::Type::A64Vec: {
    std::uint32_t value{};
    if (!reader.u32(value)) return std::nullopt;
    if (type == Dynarmic::IR::Type::A32Reg && value > 15U) {
      return std::nullopt;
    }
    if (type == Dynarmic::IR::Type::A32ExtReg && value > 79U) {
      return std::nullopt;
    }
    if ((type == Dynarmic::IR::Type::A64Reg ||
         type == Dynarmic::IR::Type::A64Vec) && value > 31U) {
      return std::nullopt;
    }
    if (type == Dynarmic::IR::Type::A32Reg) {
      return Dynarmic::IR::Value{static_cast<Dynarmic::A32::Reg>(value)};
    }
    if (type == Dynarmic::IR::Type::A32ExtReg) {
      return Dynarmic::IR::Value{static_cast<Dynarmic::A32::ExtReg>(value)};
    }
    if (type == Dynarmic::IR::Type::A64Reg) {
      return Dynarmic::IR::Value{static_cast<Dynarmic::A64::Reg>(value)};
    }
    return Dynarmic::IR::Value{static_cast<Dynarmic::A64::Vec>(value)};
  }
  case Dynarmic::IR::Type::CoprocInfo: {
    std::array<std::uint8_t, 8> value{};
    if (!reader.raw(value)) return std::nullopt;
    return Dynarmic::IR::Value{
        Dynarmic::IR::Value::CoprocessorInfo{value}};
  }
  case Dynarmic::IR::Type::NZCVFlags:
    return Dynarmic::IR::Value::EmptyNZCVImmediateMarker();
  case Dynarmic::IR::Type::Cond: {
    std::uint8_t value{};
    if (!reader.byte(value) || !valid_condition(value)) return std::nullopt;
    return Dynarmic::IR::Value{static_cast<Dynarmic::IR::Cond>(value)};
  }
  case Dynarmic::IR::Type::AccType: {
    std::uint8_t value{};
    if (!reader.byte(value) || !valid_acc_type(value)) return std::nullopt;
    return Dynarmic::IR::Value{static_cast<Dynarmic::IR::AccType>(value)};
  }
  default:
    return std::nullopt;
  }
}

[[nodiscard]] bool append_instruction(
    Dynarmic::IR::Block& block, Dynarmic::IR::Opcode opcode,
    const std::vector<Dynarmic::IR::Value>& args) {
  switch (args.size()) {
  case 0:
    block.AppendNewInst(opcode, {});
    return true;
  case 1:
    block.AppendNewInst(opcode, {args[0]});
    return true;
  case 2:
    block.AppendNewInst(opcode, {args[0], args[1]});
    return true;
  case 3:
    block.AppendNewInst(opcode, {args[0], args[1], args[2]});
    return true;
  case 4:
    block.AppendNewInst(opcode, {args[0], args[1], args[2], args[3]});
    return true;
  default:
    return false;
  }
}

} // namespace

std::optional<std::vector<std::byte>> serialize_dynarmic_ir(
    const Dynarmic::IR::Block& block) {
  Writer writer;
  writer.raw(magic);
  writer.u32(format_version);
  write_location(writer, block.Location());
  write_location(writer, block.EndLocation());
  writer.byte(static_cast<std::uint8_t>(block.GetCondition()));
  writer.byte(block.HasConditionFailedLocation() ? 1U : 0U);
  if (block.HasConditionFailedLocation()) {
    write_location(writer, block.ConditionFailedLocation());
  }
  writer.u64(static_cast<std::uint64_t>(block.ConditionFailedCycleCount()));
  writer.u64(static_cast<std::uint64_t>(block.CycleCount()));

  if (block.size() > maximum_instructions) return std::nullopt;
  writer.u32(static_cast<std::uint32_t>(block.size()));
  std::unordered_map<const Dynarmic::IR::Inst*, std::uint32_t> indices;
  indices.reserve(block.size());
  std::uint32_t index = 0;
  for (const auto& instruction : block) {
    indices.emplace(&instruction, index++);
  }
  for (const auto& instruction : block) {
    writer.u32(static_cast<std::uint32_t>(instruction.GetOpcode()));
    writer.u32(instruction.GetName());
    const auto argument_count = instruction.NumArgs();
    if (argument_count > Dynarmic::IR::max_arg_count) return std::nullopt;
    writer.byte(static_cast<std::uint8_t>(argument_count));
    for (std::size_t argument = 0; argument < argument_count; ++argument) {
      const auto encoded = encode_value(instruction.GetArg(argument), indices);
      if (!encoded) return std::nullopt;
      write_encoded_value(writer, *encoded);
    }
  }
  write_terminal(writer, block.GetTerminal(), 0);
  if (!writer.valid()) return std::nullopt;
  return std::move(writer).take();
}

std::optional<Dynarmic::IR::Block> deserialize_dynarmic_ir(
    std::span<const std::byte> bytes) {
  if (bytes.empty() || bytes.size() > maximum_bytes) return std::nullopt;
  Reader reader{bytes};
  std::array<std::uint8_t, magic.size()> actual_magic{};
  if (!reader.raw(actual_magic) || actual_magic != magic) return std::nullopt;
  std::uint32_t version{};
  if (!reader.u32(version) || version != format_version) return std::nullopt;

  Dynarmic::IR::LocationDescriptor location{0};
  Dynarmic::IR::LocationDescriptor end_location{0};
  if (!read_location(reader, location) || !read_location(reader, end_location)) {
    return std::nullopt;
  }
  std::uint8_t condition{};
  std::uint8_t has_condition_failed{};
  if (!reader.byte(condition) || !valid_condition(condition) ||
      !reader.byte(has_condition_failed) || has_condition_failed > 1U) {
    return std::nullopt;
  }
  std::optional<Dynarmic::IR::LocationDescriptor> condition_failed;
  if (has_condition_failed != 0) {
    Dynarmic::IR::LocationDescriptor value{0};
    if (!read_location(reader, value)) return std::nullopt;
    condition_failed = value;
  }
  std::uint64_t condition_failed_cycles{};
  std::uint64_t cycles{};
  if (!reader.u64(condition_failed_cycles) || !reader.u64(cycles) ||
      condition_failed_cycles > std::numeric_limits<std::size_t>::max() ||
      cycles > std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  std::uint32_t instruction_count{};
  if (!reader.u32(instruction_count) ||
      instruction_count > maximum_instructions) {
    return std::nullopt;
  }

  Dynarmic::IR::Block block{location};
  block.SetEndLocation(end_location);
  block.SetCondition(static_cast<Dynarmic::IR::Cond>(condition));
  if (condition_failed) block.SetConditionFailedLocation(*condition_failed);
  block.ConditionFailedCycleCount() =
      static_cast<std::size_t>(condition_failed_cycles);
  block.CycleCount() = static_cast<std::size_t>(cycles);

  std::vector<Dynarmic::IR::Inst*> instructions;
  instructions.reserve(instruction_count);
  for (std::uint32_t index = 0; index < instruction_count; ++index) {
    std::uint32_t raw_opcode{};
    std::uint32_t name{};
    std::uint8_t argument_count{};
    if (!reader.u32(raw_opcode) || !reader.u32(name) ||
        !reader.byte(argument_count) ||
        raw_opcode >= Dynarmic::IR::OpcodeCount ||
        argument_count != Dynarmic::IR::GetNumArgsOf(
                              static_cast<Dynarmic::IR::Opcode>(raw_opcode)) ||
        argument_count > Dynarmic::IR::max_arg_count) {
      return std::nullopt;
    }
    const auto opcode = static_cast<Dynarmic::IR::Opcode>(raw_opcode);
    std::vector<Dynarmic::IR::Value> args;
    args.reserve(argument_count);
    for (std::uint8_t argument = 0; argument < argument_count; ++argument) {
      auto value = read_value(reader, instructions, index);
      if (!value || !Dynarmic::IR::AreTypesCompatible(
                         value->GetType(),
                         Dynarmic::IR::GetArgTypeOf(opcode, argument))) {
        return std::nullopt;
      }
      args.push_back(std::move(*value));
    }
    if (!append_instruction(block, opcode, args)) return std::nullopt;
    auto& instruction = block.back();
    instruction.SetName(name);
    instructions.push_back(&instruction);
  }

  auto terminal = read_terminal(reader, 0);
  if (!terminal || !reader.at_end() || terminal->which() == 0) {
    return std::nullopt;
  }
  block.SetTerminal(std::move(*terminal));
  const auto initial_location =
      Dynarmic::A32::LocationDescriptor{block.Location()};
  if (!canonical_a32_location(block.Location()) ||
      !canonical_a32_location(block.EndLocation()) ||
      (block.GetCondition() == Dynarmic::IR::Cond::AL &&
       block.HasConditionFailedLocation()) ||
      (block.GetCondition() != Dynarmic::IR::Cond::AL &&
       !block.HasConditionFailedLocation()) ||
      (block.HasConditionFailedLocation() &&
       !canonical_a32_location(block.ConditionFailedLocation())) ||
      !valid_terminal(block.GetTerminal(), initial_location)) {
    return std::nullopt;
  }
  return block;
}

bool validate_dynarmic_ir(std::span<const std::byte> bytes) {
  return deserialize_dynarmic_ir(bytes).has_value();
}

} // namespace ilemu
