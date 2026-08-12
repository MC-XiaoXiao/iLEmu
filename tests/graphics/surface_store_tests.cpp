#include "suite.hpp"

#include "test_support.hpp"

#include "ilemu/address_space.hpp"
#include "ilemu/surface_store.hpp"

#include <cstdint>
#include <optional>

namespace ilemu::test {
namespace {

void cross_process_surface_mapping_test() {
  constexpr std::uint32_t producer_base = 0x10000U;
  constexpr std::uint32_t consumer_mapping = 0x20000U;
  constexpr std::uint32_t pixel_offset = 128U;
  constexpr std::uint32_t initial_pixel = 0xff12'3456U;
  constexpr std::uint32_t updated_pixel = 0xffab'cdefU;

  AddressSpace producer_memory;
  AddressSpace consumer_memory;
  require(producer_memory.map(
              producer_base, AddressSpace::page_size,
              MemoryPermission::Read | MemoryPermission::Write) &&
              producer_memory.write32(producer_base + pixel_offset,
                                      initial_pixel),
          "producer CoreSurface memory setup failed");

  SurfaceStore producer;
  const auto identifier = producer.allocate_identifier();
  require(producer.publish(
              producer_memory,
              SurfaceStore::Backing{
                  identifier, producer_base, AddressSpace::page_size, 32U,
                  32U, 32U * sizeof(std::uint32_t),
                  surface_pixel_format_bgra}),
          "producer CoreSurface publication failed");

  SurfaceStore consumer;
  consumer.inherit_state(producer);
  consumer.reset();
  const auto shared = consumer.shared_mapping(identifier);
  require(shared && shared->mapping_size == AddressSpace::page_size,
          "consumer did not inherit the CoreSurface registry");
  const auto imported =
      consumer.import(consumer_memory, identifier, consumer_mapping);
  require(imported && imported->base == consumer_mapping &&
              consumer_memory.read32(consumer_mapping + pixel_offset) ==
                  std::optional<std::uint32_t>{initial_pixel},
          "consumer did not map the producer CoreSurface pages");
  require(consumer_memory.write32(consumer_mapping + pixel_offset,
                                  updated_pixel) &&
              producer_memory.read32(producer_base + pixel_offset) ==
                  std::optional<std::uint32_t>{updated_pixel},
          "CoreSurface imported mapping did not preserve shared writes");
}

} // namespace

void run_surface_store_tests() { cross_process_surface_mapping_test(); }

} // namespace ilemu::test
