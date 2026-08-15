#include "ilemu/userland_hle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/macho.hpp"
#include "ilemu/output.hpp"
#include "ilemu/performance.hpp"

namespace ilemu {
namespace {

constexpr std::uint32_t arm_svc_opcode = 0xef000000U;
constexpr std::uint32_t arm_thumb_state_bit = 1U << 5U;
constexpr std::uint16_t continuation_hle_call = userland_hle_call_mask;
constexpr std::uint16_t thread_callback_return_hle_call =
    userland_hle_call_mask - 1U;
constexpr std::string_view continuation_symbol{"<guest-continuation>"};
constexpr std::string_view thread_callback_symbol{"<guest-thread-callback>"};
constexpr std::uint32_t first_string_page_candidate = 0x3fff0000U;
constexpr std::uint32_t lowest_string_page_candidate = 0x3f000000U;
constexpr std::uint32_t first_data_page_candidate = 0x50000000U;
constexpr std::uint32_t data_region_end = 0x60000000U;
constexpr std::size_t maximum_traced_hle_symbols = 512;

bool path_has_suffix(std::string_view path, std::string_view suffix) {
  return path.size() >= suffix.size() &&
         path.substr(path.size() - suffix.size()) == suffix;
}

std::string_view hle_subsystem(std::string_view image_suffix) {
  const auto separator = image_suffix.find_last_of('/');
  auto name = separator == std::string_view::npos
                  ? image_suffix
                  : image_suffix.substr(separator + 1U);
  if (name.ends_with(".dylib"))
    name.remove_suffix(6U);
  return name.empty() ? std::string_view{"unknown"} : name;
}

// Temporary frame-hitch diagnostic. Split MBX calls by their firmware symbol
// while preserving the normal subsystem aggregation for every other HLE.
std::string_view hle_performance_bucket(std::string_view image_suffix,
                                        std::string_view symbol) {
  if (symbol.starts_with("_mbx") || symbol.starts_with("_CoreSurface") ||
      symbol.starts_with("__coreSurface") ||
      symbol.starts_with("_IOMobileFramebuffer")) {
    return symbol;
  }
  return hle_subsystem(image_suffix);
}

std::array<std::byte, 4> little_endian_word(std::uint32_t value) {
  return {
      static_cast<std::byte>(value & 0xffU),
      static_cast<std::byte>((value >> 8U) & 0xffU),
      static_cast<std::byte>((value >> 16U) & 0xffU),
      static_cast<std::byte>((value >> 24U) & 0xffU),
  };
}

std::array<std::byte, 2> little_endian_halfword(std::uint16_t value) {
  return {
      static_cast<std::byte>(value & 0xffU),
      static_cast<std::byte>((value >> 8U) & 0xffU),
  };
}

std::uint16_t halfword_from_little_endian(std::span<const std::byte, 2> bytes) {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint16_t>(bytes[0]) |
      (std::to_integer<std::uint16_t>(bytes[1]) << 8U));
}

std::uint32_t word_from_little_endian(std::span<const std::byte, 4> bytes) {
  return std::to_integer<std::uint32_t>(bytes[0]) |
         (std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[3]) << 24U);
}

bool thumb_instruction_is_32_bit(std::uint16_t instruction) noexcept {
  // Thumb-2 instructions have one of the 11101, 11110, or 11111 first
  // halfword prefixes. Thumb-1's unconditional branch occupies 11100.
  return (instruction & 0xf800U) >= 0xe800U;
}

std::size_t thumb_patch_size(const MachOImage &image, std::uint32_t address,
                             ArmArchitectureVersion architecture) {
  if (architecture != ArmArchitectureVersion::Armv7)
    return sizeof(std::uint16_t);
  const auto instruction = image.read_vm_u16(address & ~1U);
  return instruction && thumb_instruction_is_32_bit(*instruction)
             ? 2U * sizeof(std::uint16_t)
             : sizeof(std::uint16_t);
}

std::vector<std::byte> make_thumb_hle_patch(std::size_t patch_size) {
  const auto svc = little_endian_halfword(
      static_cast<std::uint16_t>(0xdf00U | userland_hle_thumb_svc));
  std::vector<std::byte> patch{svc.begin(), svc.end()};
  if (patch_size == 4U) {
    const auto nop = little_endian_halfword(0xbf00U);
    patch.insert(patch.end(), nop.begin(), nop.end());
  }
  return patch;
}

std::vector<std::byte>
make_persistent_arm_trampoline(std::span<const std::byte, 4> original,
                               std::uint32_t entry) {
  const auto instruction = word_from_little_endian(original);
  constexpr std::uint32_t literal_load_mask = 0x0f7f0000U;
  constexpr std::uint32_t literal_word_load = 0x051f0000U;
  const auto target_register = (instruction >> 12U) & 0xfU;
  if ((instruction & literal_load_mask) == literal_word_load &&
      target_register != 15U) {
    const auto condition = instruction & 0xf0000000U;
    const auto immediate = instruction & 0xfffU;
    const auto original_pc = entry + 8U;
    const auto literal_address = (instruction & (1U << 23U)) != 0U
                                     ? original_pc + immediate
                                     : original_pc - immediate;
    // A PC-relative first instruction cannot simply be copied to the
    // trampoline: its literal address would move with the PC. Load the
    // original absolute literal address first, then execute the equivalent
    // register-indirect load before returning to the second instruction.
    const std::array words{
        condition | 0x059f0008U | (target_register << 12U),
        condition | 0x05900000U | (target_register << 16U) |
            (target_register << 12U),
        0xe59ff004U,
        0xe1a00000U,
        literal_address,
        entry + 4U,
    };
    std::vector<std::byte> code;
    code.reserve(words.size() * sizeof(std::uint32_t));
    for (const auto word : words) {
      const auto encoded = little_endian_word(word);
      code.insert(code.end(), encoded.begin(), encoded.end());
    }
    return code;
  }

  std::vector<std::byte> code;
  code.reserve(3U * sizeof(std::uint32_t));
  code.insert(code.end(), original.begin(), original.end());
  const auto jump = little_endian_word(0xe51ff004U);
  const auto target = little_endian_word(entry + 4U);
  code.insert(code.end(), jump.begin(), jump.end());
  code.insert(code.end(), target.begin(), target.end());
  return code;
}

std::vector<std::byte>
make_persistent_thumb_trampoline(std::span<const std::byte> original,
                                 std::uint32_t entry) {
  const auto append_halfword = [](std::vector<std::byte> &code,
                                  std::uint16_t value) {
    const auto encoded = little_endian_halfword(value);
    code.insert(code.end(), encoded.begin(), encoded.end());
  };
  const auto append_word = [](std::vector<std::byte> &code,
                              std::uint32_t value) {
    const auto encoded = little_endian_word(value);
    code.insert(code.end(), encoded.begin(), encoded.end());
  };
  const auto append_arm_return = [&](std::vector<std::byte> &code,
                                     std::uint32_t target) {
    // BX pc enters ARM state at the next aligned word. The ARM literal load
    // then returns to the requested Thumb address without borrowing a guest
    // register or changing condition flags.
    append_halfword(code, 0x4778U); // bx pc
    append_halfword(code, 0x46c0U); // nop / ARM alignment
    append_word(code, 0xe51ff004U); // ldr pc, [pc, #-4]
    append_word(code, target | 1U);
  };

  if (original.size() == 4U) {
    // ARMv7 may start a Thumb entry with a Thumb-2 instruction. Keep both
    // halfwords together; resuming at entry + 2 would execute the second
    // halfword as an unrelated instruction and can corrupt the guest stack.
    std::vector<std::byte> code;
    code.reserve(original.size() + 3U * sizeof(std::uint32_t));
    code.insert(code.end(), original.begin(), original.end());
    append_arm_return(code, entry + 4U);
    return code;
  }

  if (original.size() != sizeof(std::uint16_t))
    return {};
  const auto instruction = halfword_from_little_endian(
      std::span<const std::byte, 2>{original.data(), 2U});

  constexpr std::uint16_t literal_load_mask = 0xf800U;
  constexpr std::uint16_t literal_word_load = 0x4800U;
  if ((instruction & literal_load_mask) == literal_word_load) {
    const auto target_register = (instruction >> 8U) & 0x7U;
    const auto original_pc = (entry + 4U) & ~3U;
    const auto literal_address = original_pc + ((instruction & 0xffU) << 2U);
    std::vector<std::byte> code;
    code.reserve(5U * sizeof(std::uint32_t));
    // Load the original absolute literal address into the destination, then
    // perform the load that the relocated Thumb instruction intended.
    append_halfword(code, static_cast<std::uint16_t>(
                              0x4800U | (target_register << 8U) | 3U));
    append_halfword(code, static_cast<std::uint16_t>(0x6800U |
                                                     (target_register << 3U) |
                                                     target_register));
    append_arm_return(code, entry + 2U);
    append_word(code, literal_address);
    return code;
  }

  constexpr std::uint16_t address_load_mask = 0xf800U;
  constexpr std::uint16_t address_load = 0xa000U;
  if ((instruction & address_load_mask) == address_load) {
    const auto target_register = (instruction >> 8U) & 0x7U;
    const auto original_pc = (entry + 4U) & ~3U;
    const auto address = original_pc + ((instruction & 0xffU) << 2U);
    std::vector<std::byte> code;
    code.reserve(5U * sizeof(std::uint32_t));
    append_halfword(code, static_cast<std::uint16_t>(
                              0x4800U | (target_register << 8U) | 3U));
    append_halfword(code, 0x46c0U); // nop
    append_arm_return(code, entry + 2U);
    append_word(code, address);
    return code;
  }

  constexpr std::uint16_t unconditional_branch_mask = 0xf800U;
  constexpr std::uint16_t unconditional_branch = 0xe000U;
  if ((instruction & unconditional_branch_mask) == unconditional_branch) {
    auto displacement = static_cast<std::int32_t>(instruction & 0x7ffU) << 1U;
    if (displacement >= 0x800)
      displacement -= 0x1000;
    std::vector<std::byte> code;
    code.reserve(3U * sizeof(std::uint32_t));
    append_arm_return(
        code, static_cast<std::uint32_t>(static_cast<std::int64_t>(entry + 4U) +
                                         displacement));
    return code;
  }

  constexpr std::uint16_t conditional_branch_mask = 0xf000U;
  constexpr std::uint16_t conditional_branch = 0xd000U;
  const auto condition = (instruction >> 8U) & 0xfU;
  if ((instruction & conditional_branch_mask) == conditional_branch &&
      condition < 0xeU) {
    auto displacement = static_cast<std::int32_t>(instruction & 0xffU) << 1U;
    if (displacement >= 0x100)
      displacement -= 0x200;
    const auto taken = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(entry + 4U) + displacement);
    std::vector<std::byte> code;
    code.reserve(7U * sizeof(std::uint32_t));
    append_halfword(code,
                    static_cast<std::uint16_t>(0xd006U | (condition << 8U)));
    append_halfword(code, 0x46c0U); // nop
    append_arm_return(code, entry + 2U);
    append_arm_return(code, taken);
    return code;
  }

  std::vector<std::byte> code;
  code.reserve(4U * sizeof(std::uint32_t));
  code.insert(code.end(), original.begin(), original.end());
  append_halfword(code, 0x46c0U); // align the state transition
  append_arm_return(code, entry + 2U);
  return code;
}

bool append_utf8(std::string &output, std::uint32_t codepoint) {
  if (codepoint <= 0x7fU) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else if (codepoint <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else if (codepoint <= 0x10ffffU) {
    output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else {
    return false;
  }
  return true;
}

std::optional<std::string>
read_utf16_string(const AddressSpace &memory, std::uint32_t address,
                  std::size_t maximum_size,
                  std::optional<std::size_t> expected_units = std::nullopt) {
  std::string result;
  const auto limit = expected_units.value_or(maximum_size);
  if (limit > maximum_size)
    return std::nullopt;
  for (std::size_t index = 0; index < limit; ++index) {
    const auto offset = static_cast<std::uint64_t>(index) * 2U;
    if (offset > std::numeric_limits<std::uint32_t>::max() - address)
      return std::nullopt;
    const auto first =
        memory.read16(address + static_cast<std::uint32_t>(offset));
    if (!first)
      return std::nullopt;
    if (*first == 0)
      return expected_units ? std::nullopt : std::optional{result};
    std::uint32_t codepoint = *first;
    if (*first >= 0xd800U && *first <= 0xdbffU) {
      if (++index >= limit)
        return std::nullopt;
      const auto second_offset = static_cast<std::uint64_t>(index) * 2U;
      if (second_offset > std::numeric_limits<std::uint32_t>::max() - address) {
        return std::nullopt;
      }
      const auto second =
          memory.read16(address + static_cast<std::uint32_t>(second_offset));
      if (!second || *second < 0xdc00U || *second > 0xdfffU)
        return std::nullopt;
      codepoint = 0x10000U +
                  ((static_cast<std::uint32_t>(*first) - 0xd800U) << 10U) +
                  (static_cast<std::uint32_t>(*second) - 0xdc00U);
    } else if (*first >= 0xdc00U && *first <= 0xdfffU) {
      return std::nullopt;
    }
    if (!append_utf8(result, codepoint) || result.size() > maximum_size)
      return std::nullopt;
  }
  return expected_units ? std::optional{result} : std::nullopt;
}

} // namespace

UserlandHleCall::UserlandHleCall(UserlandHleRegistry &registry, Cpu &cpu,
                                 AddressSpace &memory, Output &output,
                                 std::uint32_t process_id,
                                 std::string_view symbol)
    : registry_{registry}, cpu_{cpu}, memory_{memory}, output_{output},
      process_id_{process_id}, symbol_{symbol} {}

std::uint32_t UserlandHleCall::argument(std::size_t index) const {
  const auto &registers = cpu_.registers();
  if (index < 4)
    return registers[index];
  const auto stack_offset = static_cast<std::uint64_t>(index - 4U) * 4U;
  if (stack_offset >
      std::numeric_limits<std::uint32_t>::max() - registers[13]) {
    return 0;
  }
  return memory_
      .read32(registers[13] + static_cast<std::uint32_t>(stack_offset))
      .value_or(0);
}

std::optional<std::string>
UserlandHleCall::string_argument(std::size_t index,
                                 std::size_t maximum_size) const {
  const auto address = argument(index);
  return address == 0 ? std::nullopt
                      : memory_.read_c_string(address, maximum_size);
}

std::optional<std::string>
UserlandHleCall::objc_string_argument(std::size_t index,
                                      std::size_t maximum_size) const {
  const auto object = argument(index);
  if (object == 0 || maximum_size == 0)
    return std::nullopt;

  // Objective-C 1.x NSString objects used by the firmware have either an
  // external byte/UTF-16 buffer with a length word, or an inline null-
  // terminated UTF-16 payload immediately after the isa/info words.
  const auto data = memory_.read32(object + 8U);
  const auto length = memory_.read32(object + 12U);
  if (data && length && *data != 0 && *length <= maximum_size) {
    if (const auto bytes = memory_.read_bytes(*data, *length)) {
      const auto embedded_null =
          std::find(bytes->begin(), bytes->end(), std::byte{0});
      if (embedded_null == bytes->end()) {
        return std::string{reinterpret_cast<const char *>(bytes->data()),
                           bytes->size()};
      }
    }
    if (const auto unicode =
            read_utf16_string(memory_, *data, maximum_size, *length)) {
      return unicode;
    }
  }
  return read_utf16_string(memory_, object + 8U, maximum_size);
}

bool UserlandHleCall::write32(std::uint32_t address, std::uint32_t value) {
  return address == 0 || memory_.write32(address, value);
}

std::uint32_t UserlandHleCall::intern_string(std::string_view value) {
  return registry_.intern_string(value);
}

std::uint32_t UserlandHleCall::allocate_data(std::size_t size,
                                             std::size_t alignment) {
  return registry_.allocate_data(size, alignment);
}

std::optional<std::uint32_t>
UserlandHleCall::symbol_address(std::string_view symbol) const {
  return registry_.symbol_address(symbol);
}

bool UserlandHleCall::image_loaded(std::string_view image_suffix) const {
  return registry_.image_loaded(image_suffix);
}

bool UserlandHleCall::image_loaded_beneath(std::string_view directory) const {
  return registry_.image_loaded_beneath(directory);
}

void UserlandHleCall::set_return(std::uint32_t value) {
  cpu_.registers()[0] = value;
}

bool UserlandHleCall::tail_call_registered(std::string_view symbol) {
  const auto address = registry_.symbol_address(symbol);
  if (!address)
    return false;
  const auto installed = registry_.installed_calls_.find(*address);
  if (installed == registry_.installed_calls_.end())
    return false;
  tail_call_address_ = *address | (installed->second.thumb ? 1U : 0U);
  return true;
}

bool UserlandHleCall::call_guest_function(std::string_view symbol,
                                          Continuation continuation) {
  if (!continuation)
    return false;
  const auto address = registry_.symbol_address(symbol);
  const auto thumb = registry_.installed_symbol_thumb_.find(symbol);
  if (!address || thumb == registry_.installed_symbol_thumb_.end()) {
    return false;
  }
  const auto return_gate = registry_.install_continuation(
      cpu_, cpu_.registers()[14], std::move(continuation));
  if (!return_gate)
    return false;
  cpu_.registers()[14] = *return_gate;
  tail_call_address_ = *address | (thumb->second ? 1U : 0U);
  return true;
}

bool UserlandHleCall::defer_guest_function(std::string_view symbol,
                                           Continuation setup,
                                           Continuation completion) {
  return registry_.defer_guest_function(symbol, cpu_.processor_id(), true,
                                        std::move(setup),
                                        std::move(completion));
}

bool UserlandHleCall::continue_deferred_guest_function(
    std::string_view symbol, Continuation setup, Continuation completion) {
  return registry_.defer_guest_function(symbol, cpu_.processor_id(), false,
                                        std::move(setup),
                                        std::move(completion));
}

void UserlandHleCall::resume_original() { resume_original_ = true; }

void UserlandHleCall::resume_original_persistently() {
  resume_original_persistently_ = true;
}

void UserlandHleCall::resume_original_persistently(Continuation continuation) {
  resume_original_persistently_ = true;
  original_continuation_ = std::move(continuation);
}

UserlandHleRegistry::UserlandHleRegistry(AddressSpace &memory, Output &output)
    : memory_{memory}, output_{output} {}

void UserlandHleRegistry::register_function(std::string image_suffix,
                                            std::string symbol,
                                            Handler handler) {
  if (!handler || registrations_.size() >= userland_hle_call_mask) {
    throw std::runtime_error{"invalid or exhausted userspace HLE registration"};
  }
  const auto duplicate =
      std::find_if(registrations_.begin(), registrations_.end(),
                   [&](const Registration &registration) {
                     return !registration.prefix &&
                            registration.image_suffix == image_suffix &&
                            registration.symbol == symbol;
                   });
  if (duplicate != registrations_.end()) {
    throw std::runtime_error{"duplicate userspace HLE function: " + symbol};
  }
  registrations_.push_back(
      Registration{static_cast<std::uint16_t>(registrations_.size() + 1U),
                   std::move(image_suffix), std::move(symbol), false,
                   std::nullopt, std::nullopt, false, std::move(handler)});
}

void UserlandHleRegistry::register_prefix(std::string image_suffix,
                                          std::string symbol_prefix,
                                          Handler handler) {
  if (!handler || registrations_.size() >= userland_hle_call_mask) {
    throw std::runtime_error{"invalid or exhausted userspace HLE registration"};
  }
  registrations_.push_back(
      Registration{static_cast<std::uint16_t>(registrations_.size() + 1U),
                   std::move(image_suffix), std::move(symbol_prefix), true,
                   std::nullopt, std::nullopt, false, std::move(handler)});
}

void UserlandHleRegistry::register_guest_function(std::string image_suffix,
                                                  std::string symbol) {
  if (image_suffix.empty() || symbol.empty()) {
    throw std::runtime_error{"invalid guest function registration"};
  }
  const auto dependency = std::pair{std::move(image_suffix), std::move(symbol)};
  if (std::find(guest_functions_.begin(), guest_functions_.end(), dependency) !=
      guest_functions_.end()) {
    throw std::runtime_error{"duplicate guest function registration: " +
                             dependency.second};
  }
  guest_functions_.push_back(dependency);
}

void UserlandHleRegistry::register_objc_instance_method(
    std::string image_suffix, std::string class_name, std::string selector,
    std::string diagnostic_name, Handler handler) {
  if (!handler || class_name.empty() || selector.empty() ||
      diagnostic_name.empty() ||
      registrations_.size() >= userland_hle_call_mask) {
    throw std::runtime_error{
        "invalid or exhausted userspace Objective-C HLE registration"};
  }
  const auto duplicate =
      std::find_if(registrations_.begin(), registrations_.end(),
                   [&](const Registration &registration) {
                     return registration.image_suffix == image_suffix &&
                            registration.objc_instance_method ==
                                std::optional{std::pair{class_name, selector}};
                   });
  if (duplicate != registrations_.end()) {
    throw std::runtime_error{"duplicate userspace Objective-C method: " +
                             diagnostic_name};
  }
  registrations_.push_back(Registration{
      static_cast<std::uint16_t>(registrations_.size() + 1U),
      std::move(image_suffix), std::move(diagnostic_name), false, std::nullopt,
      std::pair{std::move(class_name), std::move(selector)}, false,
      std::move(handler)});
}

void UserlandHleRegistry::register_objc_class_method(
    std::string image_suffix, std::string class_name, std::string selector,
    std::string diagnostic_name, Handler handler) {
  if (!handler || class_name.empty() || selector.empty() ||
      diagnostic_name.empty() ||
      registrations_.size() >= userland_hle_call_mask) {
    throw std::runtime_error{
        "invalid or exhausted userspace Objective-C HLE registration"};
  }
  const auto duplicate =
      std::find_if(registrations_.begin(), registrations_.end(),
                   [&](const Registration &registration) {
                     return registration.image_suffix == image_suffix &&
                            registration.objc_class_method &&
                            registration.objc_instance_method ==
                                std::optional{std::pair{class_name, selector}};
                   });
  if (duplicate != registrations_.end()) {
    throw std::runtime_error{"duplicate userspace Objective-C method: " +
                             diagnostic_name};
  }
  registrations_.push_back(Registration{
      static_cast<std::uint16_t>(registrations_.size() + 1U),
      std::move(image_suffix), std::move(diagnostic_name), false, std::nullopt,
      std::pair{std::move(class_name), std::move(selector)}, true,
      std::move(handler)});
}

void UserlandHleRegistry::register_address(std::string image_suffix,
                                           std::uint32_t virtual_address,
                                           std::string diagnostic_name,
                                           Handler handler) {
  if (!handler || virtual_address == 0 || diagnostic_name.empty() ||
      registrations_.size() >= userland_hle_call_mask) {
    throw std::runtime_error{
        "invalid or exhausted userspace address HLE registration"};
  }
  const auto duplicate =
      std::find_if(registrations_.begin(), registrations_.end(),
                   [&](const Registration &registration) {
                     return registration.image_suffix == image_suffix &&
                            registration.virtual_address == virtual_address;
                   });
  if (duplicate != registrations_.end()) {
    throw std::runtime_error{"duplicate userspace HLE address: " +
                             diagnostic_name};
  }
  registrations_.push_back(
      Registration{static_cast<std::uint16_t>(registrations_.size() + 1U),
                   std::move(image_suffix), std::move(diagnostic_name), false,
                   virtual_address, std::nullopt, false, std::move(handler)});
}

UserlandHleRegistry::Registration *
UserlandHleRegistry::select_registration(std::string_view image_path,
                                         std::string_view symbol) {
  Registration *prefix_match = nullptr;
  for (auto &registration : registrations_) {
    if (!path_has_suffix(image_path, registration.image_suffix))
      continue;
    if (registration.virtual_address)
      continue;
    if (registration.objc_instance_method)
      continue;
    if (!registration.prefix && registration.symbol == symbol) {
      return &registration;
    }
    if (registration.prefix && symbol.starts_with(registration.symbol) &&
        (prefix_match == nullptr ||
         registration.symbol.size() > prefix_match->symbol.size())) {
      prefix_match = &registration;
    }
  }
  return prefix_match;
}

const UserlandHleRegistry::Registration *
UserlandHleRegistry::find_registration(std::uint16_t id) const {
  if (id == 0 || id > registrations_.size())
    return nullptr;
  const auto &registration = registrations_[id - 1U];
  return registration.id == id ? &registration : nullptr;
}

const MachOImage &UserlandHleRegistry::cached_image(
    const std::filesystem::path &image_path,
    ArmArchitectureVersion architecture) {
  const auto key = image_path.generic_string();
  const auto cached = parsed_image_cache_.find(key);
  if (cached != parsed_image_cache_.end() &&
      cached->second.architecture == architecture) {
    // Mach-O parsing is cached only while the pathname still names the same
    // content. shared_file_identity() normally resolves from the parser's
    // generation-aware process cache; if a file was replaced, it recomputes
    // the identity and the next branch refreshes the parsed image.
    const auto current = shared_file_identity(image_path);
    if (current.content_identity &&
        *current.content_identity == cached->second.content_identity) {
      return *cached->second.image;
    }
  }

  auto image = std::make_shared<MachOImage>(
      MachOImage::parse(image_path, architecture));
  auto [iterator, inserted] = parsed_image_cache_.insert_or_assign(
      key, ParsedImageCacheEntry{architecture, image->content_identity(),
                                 std::move(image)});
  static_cast<void>(inserted);
  return *iterator->second.image;
}

std::size_t UserlandHleRegistry::install_mapped_image(
    Cpu &cpu, std::uint32_t process_id, const std::filesystem::path &image_path,
    std::uint32_t mapping_address, std::uint32_t mapping_size,
    std::uint64_t file_offset, ArmArchitectureVersion architecture) {
  const auto path = image_path.generic_string();
  loaded_images_.insert(path);
  if (mapping_size == 0 ||
      file_offset > std::numeric_limits<std::uint32_t>::max()) {
    return 0;
  }
  const auto hle_relevant =
      std::any_of(registrations_.begin(), registrations_.end(),
                  [&](const Registration &registration) {
                    return path_has_suffix(path, registration.image_suffix);
                  });
  const auto guest_relevant =
      std::any_of(guest_functions_.begin(), guest_functions_.end(),
                  [&](const auto &dependency) {
                    return path_has_suffix(path, dependency.first);
                  });
  if (!hle_relevant && !guest_relevant)
    return 0;

  const auto &image = cached_image(image_path, architecture);
  const auto mapping_offset = static_cast<std::uint32_t>(file_offset);
  const auto mapping_file_end =
      static_cast<std::uint64_t>(mapping_offset) + mapping_size;
  std::size_t patched = 0;
  for (const auto &symbol : image.symbols()) {
    if (symbol.value == 0)
      continue;
    const auto segment = std::find_if(
        image.segments().begin(), image.segments().end(),
        [&](const MachSegment &candidate) {
          return symbol.value >= candidate.vm_address &&
                 symbol.value - candidate.vm_address < candidate.file_size;
        });
    if (segment == image.segments().end())
      continue;
    const auto symbol_file_offset =
        static_cast<std::uint64_t>(segment->file_offset) +
        (symbol.value - segment->vm_address);
    if (symbol_file_offset < mapping_offset ||
        symbol_file_offset >= mapping_file_end) {
      continue;
    }
    const auto mapping_delta = symbol_file_offset - mapping_offset;
    if (mapping_delta >
        std::numeric_limits<std::uint32_t>::max() - mapping_address) {
      continue;
    }
    const auto runtime_address =
        mapping_address + static_cast<std::uint32_t>(mapping_delta);
    const auto guest_dependency =
        std::find_if(guest_functions_.begin(), guest_functions_.end(),
                     [&](const auto &dependency) {
                       return path_has_suffix(path, dependency.first) &&
                              dependency.second == symbol.name;
                     });
    if (hle_relevant || guest_dependency != guest_functions_.end()) {
      installed_symbols_.insert_or_assign(symbol.name, runtime_address);
      installed_symbol_thumb_.insert_or_assign(symbol.name,
                                               symbol.thumb_definition());
    }

    auto *registration = select_registration(path, symbol.name);
    if (registration == nullptr)
      continue;
    const auto patch_size = symbol.thumb_definition()
                                ? thumb_patch_size(image, symbol.value,
                                                   architecture)
                                : sizeof(std::uint32_t);
    if (symbol_file_offset + patch_size > mapping_file_end)
      continue;
    if (installed_calls_.contains(runtime_address))
      continue;
    const auto original = memory_.read_bytes(runtime_address, patch_size);
    if (!original)
      continue;
    bool copied = false;
    if (symbol.thumb_definition()) {
      // Thumb SVC has only an eight-bit immediate. The concrete handler
      // is recovered from installed_calls_ using PC-2 during dispatch.
      const auto instruction = make_thumb_hle_patch(patch_size);
      copied = memory_.copy_in(runtime_address, instruction);
      if (copied)
        cpu.invalidate_cache_range(runtime_address, instruction.size());
    } else {
      const auto instruction = little_endian_word(
          arm_svc_opcode | userland_hle_svc_namespace | registration->id);
      copied = memory_.copy_in(runtime_address, instruction);
      if (copied)
        cpu.invalidate_cache_range(runtime_address, instruction.size());
    }
    if (!copied)
      continue;
    installed_calls_.emplace(
        runtime_address, InstalledCall{registration->id, symbol.name,
                                       symbol.thumb_definition(), *original});
    ++patched;
  }
  for (const auto &registration : registrations_) {
    if (!registration.virtual_address ||
        !path_has_suffix(path, registration.image_suffix)) {
      continue;
    }
    const bool thumb = (*registration.virtual_address & 1U) != 0;
    const auto preferred_address = *registration.virtual_address & ~1U;
    const auto segment = std::find_if(
        image.segments().begin(), image.segments().end(),
        [&](const MachSegment &candidate) {
          return preferred_address >= candidate.vm_address &&
                 preferred_address - candidate.vm_address < candidate.file_size;
        });
    if (segment == image.segments().end())
      continue;
    const auto address_file_offset =
        static_cast<std::uint64_t>(segment->file_offset) +
        (preferred_address - segment->vm_address);
    const auto patch_size = thumb
                                ? thumb_patch_size(image, preferred_address,
                                                   architecture)
                                : sizeof(std::uint32_t);
    if (address_file_offset < mapping_offset ||
        address_file_offset + patch_size > mapping_file_end) {
      continue;
    }
    const auto mapping_delta = address_file_offset - mapping_offset;
    if (mapping_delta >
        std::numeric_limits<std::uint32_t>::max() - mapping_address) {
      continue;
    }
    const auto runtime_address =
        mapping_address + static_cast<std::uint32_t>(mapping_delta);
    if (installed_calls_.contains(runtime_address))
      continue;
    const auto original = memory_.read_bytes(runtime_address, patch_size);
    if (!original)
      continue;
    bool copied = false;
    if (thumb) {
      const auto instruction = make_thumb_hle_patch(patch_size);
      copied = memory_.copy_in(runtime_address, instruction);
      if (copied) {
        cpu.invalidate_cache_range(runtime_address, instruction.size());
      }
    } else {
      const auto instruction = little_endian_word(
          arm_svc_opcode | userland_hle_svc_namespace | registration.id);
      copied = memory_.copy_in(runtime_address, instruction);
      if (copied) {
        cpu.invalidate_cache_range(runtime_address, instruction.size());
      }
    }
    if (!copied)
      continue;
    installed_calls_.emplace(
        runtime_address,
        InstalledCall{registration.id, registration.symbol, thumb, *original});
    ++patched;
  }
  for (const auto &registration : registrations_) {
    if (!registration.objc_instance_method ||
        !path_has_suffix(path, registration.image_suffix)) {
      continue;
    }
    const auto &[class_name, selector] = *registration.objc_instance_method;
    const auto method =
        registration.objc_class_method
            ? image.find_objc_class_method(class_name, selector)
            : image.find_objc_instance_method(class_name, selector);
    if (!method)
      continue;
    const bool thumb = (*method & 1U) != 0;
    const auto preferred_address = *method & ~1U;
    const auto segment = std::find_if(
        image.segments().begin(), image.segments().end(),
        [&](const MachSegment &candidate) {
          return preferred_address >= candidate.vm_address &&
                 preferred_address - candidate.vm_address < candidate.file_size;
        });
    if (segment == image.segments().end())
      continue;
    const auto address_file_offset =
        static_cast<std::uint64_t>(segment->file_offset) +
        (preferred_address - segment->vm_address);
    const auto patch_size = thumb
                                ? thumb_patch_size(image, preferred_address,
                                                   architecture)
                                : sizeof(std::uint32_t);
    if (address_file_offset < mapping_offset ||
        address_file_offset + patch_size > mapping_file_end) {
      continue;
    }
    const auto mapping_delta = address_file_offset - mapping_offset;
    if (mapping_delta >
        std::numeric_limits<std::uint32_t>::max() - mapping_address) {
      continue;
    }
    const auto runtime_address =
        mapping_address + static_cast<std::uint32_t>(mapping_delta);
    if (installed_calls_.contains(runtime_address))
      continue;
    const auto original = memory_.read_bytes(runtime_address, patch_size);
    if (!original)
      continue;
    bool copied = false;
    if (thumb) {
      const auto instruction = make_thumb_hle_patch(patch_size);
      copied = memory_.copy_in(runtime_address, instruction);
      if (copied)
        cpu.invalidate_cache_range(runtime_address, instruction.size());
    } else {
      const auto instruction = little_endian_word(
          arm_svc_opcode | userland_hle_svc_namespace | registration.id);
      copied = memory_.copy_in(runtime_address, instruction);
      if (copied)
        cpu.invalidate_cache_range(runtime_address, instruction.size());
    }
    if (!copied)
      continue;
    installed_calls_.emplace(
        runtime_address,
        InstalledCall{registration.id, registration.symbol, thumb, *original});
    installed_symbols_.insert_or_assign(registration.symbol, runtime_address);
    ++patched;
  }
  if (patched != 0) {
    output_.write("[hle] installed pid=" + std::to_string(process_id) +
                  " image=" + image_path.filename().string() +
                  " functions=" + std::to_string(patched) + "\n");
  }
  return patched;
}

bool UserlandHleRegistry::dispatch(Cpu &cpu, std::uint32_t process_id,
                                   std::uint32_t svc_immediate) {
  if (!deferred_guest_calls_.empty() && svc_immediate == 0x80U &&
      deliver_deferred_guest_function(cpu, process_id, svc_immediate)) {
    return true;
  }
  const bool thumb = svc_immediate == userland_hle_thumb_svc;
  if (!thumb && (svc_immediate & userland_hle_svc_namespace_mask) !=
                    userland_hle_svc_namespace) {
    return false;
  }

  // Dynarmic exposes the architectural PC after SVC. Thumb HLEs share one
  // immediate and are selected by their two-byte entry address; ARM HLEs
  // retain the encoded registration id used by the original implementation.
  const auto entry = cpu.registers()[15] - (thumb ? 2U : 4U);
  if (!thumb &&
      (svc_immediate & userland_hle_call_mask) ==
          thread_callback_return_hle_call &&
      entry == thread_callback_return_address_) {
    const auto pending = pending_thread_callbacks_.find(cpu.processor_id());
    if (pending == pending_thread_callbacks_.end())
      return false;
    UserlandHleCall call{*this,   cpu,        memory_,
                         output_, process_id, thread_callback_symbol};
    pending->second(call);
    // A host-backed driver owns one persistent guest callback thread. Park it
    // between device periods; the scheduler restores its registers and wakes
    // the same slot when the next buffer is due.
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
    return true;
  }
  if (!thumb &&
      (svc_immediate & userland_hle_call_mask) == continuation_hle_call) {
    const auto pending = pending_continuations_.find(entry);
    if (pending == pending_continuations_.end())
      return false;
    auto continuation = std::move(pending->second);
    pending_continuations_.erase(pending);
    available_continuation_trampolines_.push_back(entry);

    auto &registers = cpu.registers();
    registers[14] = continuation.return_address;
    UserlandHleCall call{*this,   cpu,        memory_,
                         output_, process_id, continuation_symbol};
    continuation.handler(call);
    if (call.tail_call_address_) {
      const auto target = *call.tail_call_address_;
      registers[15] = target & ~1U;
      auto cpsr = cpu.cpsr();
      if ((target & 1U) != 0) {
        cpsr |= arm_thumb_state_bit;
      } else {
        cpsr &= ~arm_thumb_state_bit;
      }
      cpu.set_cpsr(cpsr);
      return true;
    }
    registers[15] = continuation.return_address & ~1U;
    auto cpsr = cpu.cpsr();
    if ((continuation.return_address & 1U) != 0) {
      cpsr |= arm_thumb_state_bit;
    } else {
      cpsr &= ~arm_thumb_state_bit;
    }
    cpu.set_cpsr(cpsr);
    return true;
  }
  const auto installed = installed_calls_.find(entry);
  const auto id =
      thumb && installed != installed_calls_.end()
          ? installed->second.id
          : static_cast<std::uint16_t>(svc_immediate & userland_hle_call_mask);
  const auto *registration = find_registration(id);
  if (registration == nullptr ||
      (thumb &&
       (installed == installed_calls_.end() || !installed->second.thumb))) {
    return false;
  }

  // The mapping supplies the concrete symbol for prefix HLEs.
  const std::string_view symbol =
      installed != installed_calls_.end()
          ? std::string_view{installed->second.symbol}
          : std::string_view{registration->symbol};
  if (traced_symbols_.size() < maximum_traced_hle_symbols &&
      !traced_symbols_.contains(symbol)) {
    traced_symbols_.emplace(symbol);
    output_.write("[hle] call pid=" + std::to_string(process_id) +
                  " cpu=" + std::to_string(cpu.processor_id()) +
                  " symbol=" + std::string{symbol} + "\n");
  }
  UserlandHleCall call{*this, cpu, memory_, output_, process_id, symbol};
  const auto measure_hle = performance_counters().enabled();
  const auto hle_start = measure_hle ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
  registration->handler(call);
  if (measure_hle) {
    const auto elapsed = std::chrono::steady_clock::now() - hle_start;
    performance_counters().record_hle(
        hle_performance_bucket(registration->image_suffix, symbol),
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                .count()));
  }

  auto &registers = cpu.registers();
  if (call.tail_call_address_) {
    const auto target = *call.tail_call_address_;
    registers[15] = target & ~1U;
    auto cpsr = cpu.cpsr();
    if ((target & 1U) != 0) {
      cpsr |= arm_thumb_state_bit;
    } else {
      cpsr &= ~arm_thumb_state_bit;
    }
    cpu.set_cpsr(cpsr);
    return true;
  }
  if (call.resume_original_persistently_) {
    const auto original_size = installed == installed_calls_.end()
                                   ? 0U
                                   : installed->second.original.size();
    const auto valid_original_size =
        installed != installed_calls_.end() &&
        (installed->second.thumb
             ? (original_size == sizeof(std::uint16_t) || original_size == 4U)
             : original_size == sizeof(std::uint32_t));
    if (!valid_original_size) {
      return false;
    }
    auto trampoline = persistent_trampolines_.find(entry);
    if (trampoline == persistent_trampolines_.end()) {
      const auto address = persistent_trampoline_cursor_;
      const auto code = installed->second.thumb
                            ? make_persistent_thumb_trampoline(std::span<const std::byte>{
                                                                  installed->second.original.data(),
                                                                  installed->second.original.size()},
                                                              entry)
                            : make_persistent_arm_trampoline(
                                  std::span<const std::byte, 4>{
                                      installed->second.original.data(), 4U},
                                  entry);
      const auto first_page = address & ~(AddressSpace::page_size - 1U);
      const auto last_page =
          (address + static_cast<std::uint32_t>(code.size()) - 1U) &
          ~(AddressSpace::page_size - 1U);
      for (auto page = first_page;; page += AddressSpace::page_size) {
        if (!memory_.mapped(page, AddressSpace::page_size) &&
            !memory_.map(page, AddressSpace::page_size,
                         MemoryPermission::Read | MemoryPermission::Write |
                             MemoryPermission::Execute)) {
          return false;
        }
        if (page == last_page)
          break;
      }
      if (!memory_.copy_in(address, code)) {
        return false;
      }
      cpu.invalidate_cache_range(address, code.size());
      trampoline = persistent_trampolines_.emplace(entry, address).first;
      persistent_trampoline_cursor_ += static_cast<std::uint32_t>(code.size());
    }
    registers[15] = trampoline->second;
    if (call.original_continuation_) {
      const auto continuation = install_continuation(
          cpu, registers[14], std::move(call.original_continuation_));
      if (!continuation)
        return false;
      registers[14] = *continuation;
    }
    auto cpsr = cpu.cpsr();
    if (installed->second.thumb) {
      cpsr |= arm_thumb_state_bit;
    } else {
      cpsr &= ~arm_thumb_state_bit;
    }
    cpu.set_cpsr(cpsr);
    return true;
  }
  if (call.resume_original_) {
    if (installed == installed_calls_.end() ||
        !memory_.copy_in(entry, installed->second.original)) {
      return false;
    }
    const bool original_thumb = installed->second.thumb;
    cpu.invalidate_cache_range(entry, installed->second.original.size());
    installed_calls_.erase(installed);
    registers[15] = entry;
    auto cpsr = cpu.cpsr();
    if (original_thumb) {
      cpsr |= arm_thumb_state_bit;
    } else {
      cpsr &= ~arm_thumb_state_bit;
    }
    cpu.set_cpsr(cpsr);
    return true;
  }

  const auto return_address = registers[14];
  registers[15] = return_address & ~1U;
  auto cpsr = cpu.cpsr();
  if ((return_address & 1U) != 0) {
    cpsr |= arm_thumb_state_bit;
  } else {
    cpsr &= ~arm_thumb_state_bit;
  }
  cpu.set_cpsr(cpsr);
  return true;
}

std::optional<std::uint32_t> UserlandHleRegistry::install_continuation(
    Cpu &cpu, std::uint32_t return_address,
    UserlandHleCall::Continuation continuation) {
  if (!continuation)
    return std::nullopt;
  std::uint32_t address{};
  if (!available_continuation_trampolines_.empty()) {
    address = available_continuation_trampolines_.back();
    available_continuation_trampolines_.pop_back();
  } else {
    address = continuation_trampoline_cursor_;
    continuation_trampoline_cursor_ += sizeof(std::uint32_t);
  }
  const auto page = address & ~(AddressSpace::page_size - 1U);
  if (!memory_.mapped(page, AddressSpace::page_size) &&
      !memory_.map(page, AddressSpace::page_size,
                   MemoryPermission::Read | MemoryPermission::Write |
                       MemoryPermission::Execute)) {
    return std::nullopt;
  }
  const auto instruction = little_endian_word(
      arm_svc_opcode | userland_hle_svc_namespace | continuation_hle_call);
  if (!memory_.copy_in(address, instruction))
    return std::nullopt;
  cpu.invalidate_cache_range(address, instruction.size());
  pending_continuations_.emplace(
      address, PendingContinuation{return_address, std::move(continuation)});
  return address;
}

bool UserlandHleRegistry::defer_guest_function(std::string_view symbol,
                                               std::size_t processor_id,
                                               bool wait_for_receive_boundary,
                                               Handler setup,
                                               Handler completion) {
  if (!setup)
    return false;
  const auto address = symbol_address(symbol);
  const auto thumb = installed_symbol_thumb_.find(symbol);
  if (!address || thumb == installed_symbol_thumb_.end())
    return false;
  deferred_guest_calls_.push_back(DeferredGuestCall{
      *address, processor_id, wait_for_receive_boundary, thumb->second,
      std::move(setup), std::move(completion)});
  return true;
}

bool UserlandHleRegistry::deliver_deferred_guest_function(
    Cpu &cpu, std::uint32_t process_id, std::uint32_t svc_immediate) {
  if (svc_immediate != 0x80U || deferred_guest_calls_.empty()) {
    return false;
  }

  // A service callback belongs at the consumer thread's event-loop boundary,
  // not at an arbitrary syscall made while the initiating UI action is still
  // unwinding. A receive-only mach_msg trap is the stable Darwin boundary at
  // which CFRunLoop is about to wait for its next source. Run the pending
  // callback there, then restore and retry the original receive.
  constexpr std::int32_t mach_message_trap = -31;
  constexpr std::uint32_t mach_receive_message = 0x2U;
  const auto &boundary_registers = cpu.registers();
  const auto at_receive_boundary =
      static_cast<std::int32_t>(boundary_registers[12]) == mach_message_trap &&
      boundary_registers[2] == 0 &&
      (boundary_registers[1] & mach_receive_message) != 0;

  const auto pending = std::find_if(
      deferred_guest_calls_.begin(), deferred_guest_calls_.end(),
      [&](const DeferredGuestCall &candidate) {
        return candidate.processor_id == cpu.processor_id() &&
               (!candidate.wait_for_receive_boundary || at_receive_boundary);
      });
  if (pending == deferred_guest_calls_.end())
    return false;
  auto deferred = std::move(*pending);
  deferred_guest_calls_.erase(pending);
  const auto saved_registers = boundary_registers;
  const auto saved_cpsr = cpu.cpsr();
  const auto interrupted_thumb = (saved_cpsr & arm_thumb_state_bit) != 0;
  const auto svc_size = interrupted_thumb ? 2U : 4U;
  const auto svc_entry = saved_registers[15] - svc_size;
  const auto return_gate =
      install_continuation(cpu, svc_entry | (interrupted_thumb ? 1U : 0U),
                           [saved_registers, saved_cpsr,
                            completion = std::move(deferred.completion)](
                               UserlandHleCall &completed) mutable {
                             if (completion)
                               completion(completed);
                             completed.cpu().registers() = saved_registers;
                             completed.cpu().set_cpsr(saved_cpsr);
                           });
  if (!return_gate) {
    deferred_guest_calls_.push_front(std::move(deferred));
    return false;
  }

  UserlandHleCall setup{*this,   cpu,        memory_,
                        output_, process_id, "__deferred_guest_setup"};
  deferred.setup(setup);
  auto &registers = cpu.registers();
  registers[14] = *return_gate;
  registers[15] = deferred.address;
  auto cpsr = cpu.cpsr();
  if (deferred.thumb) {
    cpsr |= arm_thumb_state_bit;
  } else {
    cpsr &= ~arm_thumb_state_bit;
  }
  cpu.set_cpsr(cpsr);
  return true;
}

std::optional<std::uint32_t>
UserlandHleRegistry::prepare_thread_callback_return(Cpu &cpu) {
  if (thread_callback_return_address_ != 0)
    return thread_callback_return_address_;

  constexpr std::uint32_t address = 0x62000000U;
  const auto page = address & ~(AddressSpace::page_size - 1U);
  if (!memory_.mapped(page, AddressSpace::page_size) &&
      !memory_.map(page, AddressSpace::page_size,
                   MemoryPermission::Read | MemoryPermission::Write |
                       MemoryPermission::Execute)) {
    return std::nullopt;
  }
  const auto instruction =
      little_endian_word(arm_svc_opcode | userland_hle_svc_namespace |
                         thread_callback_return_hle_call);
  if (!memory_.copy_in(address, instruction))
    return std::nullopt;
  cpu.invalidate_cache_range(address, instruction.size());
  thread_callback_return_address_ = address;
  return address;
}

std::optional<std::uint32_t> UserlandHleRegistry::prepare_one_shot_return(
    Cpu &cpu, std::uint32_t return_address, Handler completion) {
  return install_continuation(cpu, return_address, std::move(completion));
}

bool UserlandHleRegistry::bind_thread_callback(std::size_t processor,
                                               Handler completion) {
  return completion && thread_callback_return_address_ != 0 &&
         pending_thread_callbacks_.emplace(processor, std::move(completion))
             .second;
}

void UserlandHleRegistry::unbind_thread_callback(std::size_t processor) {
  pending_thread_callbacks_.erase(processor);
}

std::uint32_t UserlandHleRegistry::ensure_string_page() {
  if (string_page_ != 0 &&
      memory_.mapped(string_page_, AddressSpace::page_size)) {
    return string_page_;
  }
  for (std::uint32_t candidate = first_string_page_candidate;
       candidate >= lowest_string_page_candidate;
       candidate -= AddressSpace::page_size) {
    if (memory_.mapped(candidate, AddressSpace::page_size))
      continue;
    if (memory_.map(candidate, AddressSpace::page_size,
                    MemoryPermission::Read)) {
      string_page_ = candidate;
      string_cursor_ = candidate;
      return candidate;
    }
  }
  return 0;
}

std::uint32_t UserlandHleRegistry::intern_string(std::string_view value) {
  if (const auto existing = interned_strings_.find(value);
      existing != interned_strings_.end()) {
    return existing->second;
  }
  if (ensure_string_page() == 0 ||
      value.size() + 1U > AddressSpace::page_size ||
      string_cursor_ - string_page_ >
          AddressSpace::page_size - (value.size() + 1U)) {
    return 0;
  }
  std::vector<std::byte> bytes;
  bytes.reserve(value.size() + 1U);
  for (const auto character : value) {
    bytes.push_back(static_cast<std::byte>(character));
  }
  bytes.push_back(std::byte{});
  const auto address = string_cursor_;
  if (!memory_.copy_in(address, bytes))
    return 0;
  string_cursor_ += static_cast<std::uint32_t>(bytes.size());
  interned_strings_.emplace(value, address);
  return address;
}

std::uint32_t UserlandHleRegistry::allocate_data(std::size_t size,
                                                 std::size_t alignment) {
  if (size == 0 || size > std::numeric_limits<std::uint32_t>::max() ||
      alignment == 0 || (alignment & (alignment - 1U)) != 0 ||
      alignment > AddressSpace::page_size) {
    return 0;
  }

  const auto size32 = static_cast<std::uint32_t>(size);
  const auto alignment32 = static_cast<std::uint32_t>(alignment);
  auto candidate = data_cursor_ == 0 ? first_data_page_candidate : data_cursor_;
  while (candidate < data_region_end) {
    const auto aligned64 =
        (static_cast<std::uint64_t>(candidate) + alignment32 - 1U) &
        ~static_cast<std::uint64_t>(alignment32 - 1U);
    if (aligned64 > std::numeric_limits<std::uint32_t>::max())
      return 0;
    const auto aligned = static_cast<std::uint32_t>(aligned64);
    if (size32 > data_region_end - aligned)
      return 0;
    const auto last = aligned + size32 - 1U;
    const auto first_page = aligned & ~(AddressSpace::page_size - 1U);
    const auto last_page = last & ~(AddressSpace::page_size - 1U);

    bool collision = false;
    std::uint32_t collision_page = 0;
    for (std::uint64_t page = first_page; page <= last_page;
         page += AddressSpace::page_size) {
      const auto page32 = static_cast<std::uint32_t>(page);
      if (!data_pages_.contains(page32) && memory_.mapped(page32)) {
        collision = true;
        collision_page = page32;
        break;
      }
    }
    if (collision) {
      candidate = collision_page + AddressSpace::page_size;
      continue;
    }

    for (std::uint64_t page = first_page; page <= last_page;
         page += AddressSpace::page_size) {
      const auto page32 = static_cast<std::uint32_t>(page);
      if (data_pages_.contains(page32))
        continue;
      if (!memory_.map(page32, AddressSpace::page_size,
                       MemoryPermission::Read | MemoryPermission::Write)) {
        return 0;
      }
      data_pages_.insert(page32);
    }
    data_cursor_ = aligned + size32;
    return aligned;
  }
  return 0;
}

std::optional<std::uint32_t>
UserlandHleRegistry::symbol_address(std::string_view symbol) const {
  const auto found = installed_symbols_.find(symbol);
  return found == installed_symbols_.end()
             ? std::nullopt
             : std::optional<std::uint32_t>{found->second};
}

bool UserlandHleRegistry::image_loaded(std::string_view image_suffix) const {
  return std::any_of(loaded_images_.begin(), loaded_images_.end(),
                     [image_suffix](const std::string &image) {
                       return path_has_suffix(image, image_suffix);
                     });
}

bool UserlandHleRegistry::image_loaded_beneath(
    std::string_view directory) const {
  if (directory.empty())
    return false;
  return std::any_of(loaded_images_.begin(), loaded_images_.end(),
                     [directory](const std::string &image) {
                       const auto position = image.find(directory);
                       return position != std::string::npos &&
                              (position == 0 || image[position - 1U] == '/');
                     });
}

void UserlandHleRegistry::record_loaded_image(std::string image_path) {
  loaded_images_.insert(std::move(image_path));
}

void UserlandHleRegistry::reset_mappings() {
  installed_calls_.clear();
  installed_symbols_.clear();
  installed_symbol_thumb_.clear();
  loaded_images_.clear();
  parsed_image_cache_.clear();
  interned_strings_.clear();
  string_page_ = 0;
  string_cursor_ = 0;
  data_pages_.clear();
  data_cursor_ = 0;
  persistent_trampolines_.clear();
  persistent_trampoline_cursor_ = 0x60000000U;
  pending_continuations_.clear();
  deferred_guest_calls_.clear();
  available_continuation_trampolines_.clear();
  continuation_trampoline_cursor_ = 0x61000000U;
  thread_callback_return_address_ = 0;
  pending_thread_callbacks_.clear();
  traced_symbols_.clear();
}

void UserlandHleRegistry::inherit_mappings(const UserlandHleRegistry &parent) {
  installed_calls_ = parent.installed_calls_;
  installed_symbols_ = parent.installed_symbols_;
  installed_symbol_thumb_ = parent.installed_symbol_thumb_;
  loaded_images_ = parent.loaded_images_;
  parsed_image_cache_ = parent.parsed_image_cache_;
  interned_strings_ = parent.interned_strings_;
  string_page_ = parent.string_page_;
  string_cursor_ = parent.string_cursor_;
  data_pages_ = parent.data_pages_;
  data_cursor_ = parent.data_cursor_;
  persistent_trampolines_ = parent.persistent_trampolines_;
  persistent_trampoline_cursor_ = parent.persistent_trampoline_cursor_;
  pending_continuations_.clear();
  deferred_guest_calls_.clear();
  available_continuation_trampolines_ =
      parent.available_continuation_trampolines_;
  continuation_trampoline_cursor_ = parent.continuation_trampoline_cursor_;
  thread_callback_return_address_ = parent.thread_callback_return_address_;
  pending_thread_callbacks_.clear();
}

} // namespace ilemu
