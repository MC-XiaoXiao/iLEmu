#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/xattr.h>

#include "ilemu/address_space.hpp"
#include "ilemu/apple80211_hle.hpp"
#include "ilemu/clock_mig_ids.hpp"
#include "ilemu/clock_reply_mig_ids.hpp"
#include "ilemu/core_surface_abi.hpp"
#include "ilemu/core_surface_hle.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/device_mig_ids.hpp"
#include "ilemu/display.hpp"
#include "ilemu/dnssd_ipc_abi.hpp"
#include "ilemu/gdb_rsp.hpp"
#include "ilemu/gles_abi.hpp"
#include "ilemu/hfs_metadata.hpp"
#include "ilemu/host_network.hpp"
#include "ilemu/iokit_abi.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/kernel_iokit.hpp"
#include "ilemu/kernel_mach_ipc.hpp"
#include "ilemu/mach_clock_abi.hpp"
#include "ilemu/mach_namespace.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_port_object.hpp"
#include "ilemu/mach_scheduler_abi.hpp"
#include "ilemu/mach_thread_policy_abi.hpp"
#include "ilemu/macho.hpp"
#include "ilemu/mbx2d_abi.hpp"
#include "ilemu/mbx2d_hle.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/mobile_framebuffer_hle.hpp"
#include "ilemu/opengles_hle.hpp"
#include "ilemu/surface_store.hpp"
#include "ilemu/system_configuration_mig_ids.hpp"
#include "ilemu/userland_hle.hpp"
#include "ilemu/virtual_network.hpp"
#include "ilemu/wifi_state.hpp"
#include "ilemu/xnu_mig_adapter.hpp"
#include "ilemu/xnu_scheduler.hpp"

#include "test_support.hpp"

namespace {

using namespace ilemu;
using ilemu::test::require;

void filesystem_directory_syscall_test() {
  AddressSpace memory;
  constexpr std::uint32_t path_address = 0x3b000;
  require(memory.map(path_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "mkdir path memory map failed");
  constexpr std::string_view path{"/var/run/notify-test\0", 21};
  std::array<std::byte, path.size()> path_bytes{};
  for (std::size_t index = 0; index < path.size(); ++index) {
    path_bytes[index] = static_cast<std::byte>(path[index]);
  }
  require(memory.copy_in(path_address, path_bytes), "mkdir path copy failed");

  const auto test_directory =
      std::filesystem::temp_directory_path() / "ilemu-mkdir-tests";
  std::error_code filesystem_error;
  std::filesystem::remove_all(test_directory, filesystem_error);
  std::filesystem::create_directories(test_directory / "rootfs/var/run",
                                      filesystem_error);
  require(!filesystem_error, "mkdir test root creation failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output, test_directory / "rootfs"};
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = 0755;
  cpu.registers()[12] = 136;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "mkdir syscall failed");
  require(std::filesystem::is_directory(test_directory /
                                        "rootfs/var/run/notify-test"),
          "mkdir escaped or did not create the guest directory");
  std::filesystem::remove_all(test_directory, filesystem_error);
}

void writable_file_syscall_test() {
  AddressSpace memory;
  constexpr std::uint32_t path_address = 0x3c000;
  constexpr std::uint32_t data_address = path_address + 0x100;
  require(memory.map(path_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "writable-file memory map failed");
  constexpr std::string_view path{"/var/test.db\0", 13};
  constexpr std::string_view initial{"sqlite"};
  constexpr std::string_view replacement{"XX"};
  std::vector<std::byte> path_bytes(path.size());
  std::transform(path.begin(), path.end(), path_bytes.begin(),
                 [](char value) { return static_cast<std::byte>(value); });
  require(memory.copy_in(path_address, path_bytes),
          "writable-file path copy failed");
  std::vector<std::byte> data(initial.size());
  std::transform(initial.begin(), initial.end(), data.begin(),
                 [](char value) { return static_cast<std::byte>(value); });
  require(memory.copy_in(data_address, data), "writable-file data copy failed");

  const auto test_directory =
      std::filesystem::temp_directory_path() / "ilemu-file-tests";
  std::error_code filesystem_error;
  std::filesystem::remove_all(test_directory, filesystem_error);
  std::filesystem::create_directories(test_directory / "rootfs/var",
                                      filesystem_error);
  require(!filesystem_error, "writable-file test root creation failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output, test_directory / "rootfs"};

  cpu.registers()[0] = path_address;
  cpu.registers()[1] = 0x0200U | 0x0400U | 0x0002U; // O_CREAT|O_TRUNC|O_RDWR
  cpu.registers()[2] = 0644;
  cpu.registers()[12] = 5;
  kernel.dispatch(cpu, 0x80);
  const auto fd = cpu.registers()[0];
  require(fd >= 3, "open(O_CREAT) did not return a file descriptor");

  cpu.registers()[0] = fd;
  cpu.registers()[1] = data_address;
  cpu.registers()[2] = static_cast<std::uint32_t>(data.size());
  cpu.registers()[12] = 4;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == data.size(), "regular-file write failed");
  cpu.registers()[0] = fd;
  cpu.registers()[12] = darwin::syscall::synchronize_file;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "regular-file fsync failed");

  std::vector<std::byte> replacement_bytes(replacement.size());
  std::transform(replacement.begin(), replacement.end(),
                 replacement_bytes.begin(),
                 [](char value) { return static_cast<std::byte>(value); });
  require(memory.copy_in(data_address, replacement_bytes),
          "pwrite replacement copy failed");
  cpu.registers()[0] = fd;
  cpu.registers()[1] = data_address;
  cpu.registers()[2] = static_cast<std::uint32_t>(replacement.size());
  cpu.registers()[3] = 2;
  cpu.registers()[4] = 0;
  cpu.registers()[12] = 154;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == replacement.size(), "pwrite failed");

  std::ifstream written{test_directory / "rootfs/var/test.db",
                        std::ios::binary};
  const std::string contents{std::istreambuf_iterator<char>{written},
                             std::istreambuf_iterator<char>{}};
  require(contents == "sqXXte", "regular-file contents are incorrect");
  std::filesystem::remove_all(test_directory, filesystem_error);
}

void hfs_metadata_projection_test() {
  const auto test_directory =
      std::filesystem::temp_directory_path() / "ilemu-hfs-tests";
  const auto root = test_directory / "rootfs";
  const auto file = root / "bin/demo";
  const auto link = root / "bin/demo-link";
  std::error_code filesystem_error;
  std::filesystem::remove_all(test_directory, filesystem_error);
  std::filesystem::create_directories(root / "bin", filesystem_error);
  require(!filesystem_error, "HFS metadata test root creation failed");
  {
    std::ofstream stream{file, std::ios::binary};
    stream << "DATA";
  }
  std::filesystem::create_hard_link(file, link, filesystem_error);
  require(!filesystem_error, "HFS hard-link fixture creation failed");
  {
    std::ofstream stream{hfs::MetadataProvider::resource_sidecar(file),
                         std::ios::binary};
    stream << "RSRC";
  }
  const auto set_text_xattr = [&](std::string_view name,
                                  std::string_view value) {
    require(::setxattr(file.c_str(), std::string{name}.c_str(), value.data(),
                       value.size(), 0) == 0,
            "HFS text xattr fixture creation failed");
  };
  set_text_xattr("user.hfsfuse.record.cnid", "2213");
  set_text_xattr("user.hfsfuse.record.owner_id", "501");
  set_text_xattr("user.hfsfuse.record.group_id", "501");
  set_text_xattr("user.hfsfuse.record.file_mode", "33261");
  set_text_xattr("user.hfsfuse.record.bsd_flags", "2");
  set_text_xattr("user.hfsfuse.record.date_created",
                 "2007-05-27T12:34:56+0000");
  std::array<std::byte, 32> finder_info{};
  finder_info[0] = std::byte{0x54};
  finder_info[1] = std::byte{0x45};
  require(::setxattr(file.c_str(), "user.com.apple.FinderInfo",
                     finder_info.data(), finder_info.size(), 0) == 0,
          "HFS FinderInfo fixture creation failed");

  hfs::MetadataProvider provider{root};
  const auto metadata = provider.query(file, true);
  const auto link_metadata = provider.query(link, true);
  require(metadata && link_metadata,
          "HFS metadata provider could not inspect fixture files");
  require(metadata->catalog_id != link_metadata->catalog_id &&
              metadata->permanent_id == link_metadata->permanent_id,
          "HFS hard link did not separate catalog and inode identities");
  require(metadata->owner == 501 && metadata->group == 501 &&
              metadata->mode == 0100755U && metadata->flags == 2 &&
              metadata->creation_time.seconds == 1'180'269'296 &&
              metadata->finder_info[0] == std::byte{0x54} &&
              metadata->resource_length == 4,
          "HFS metadata xattrs were not projected into vnode metadata");

  using namespace hfs::attribute;
  const hfs::AttributeRequest request{
      common_name | common_object_id | common_creation_time |
          common_finder_info | common_owner_id | common_flags,
      0, 0, file_total_size | file_data_length | file_resource_length, 0};
  require(hfs::MetadataProvider::valid_request(request),
          "valid Darwin 8 getattrlist mask was rejected");
  const auto packed =
      hfs::MetadataProvider::pack_attributes(*metadata, request);
  const auto packed32 = [&](std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
      value |= std::to_integer<std::uint32_t>(packed[offset + byte])
               << (byte * 8U);
    }
    return value;
  };
  const auto packed64 = [&](std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < 8; ++byte) {
      value |= std::to_integer<std::uint64_t>(packed[offset + byte])
               << (byte * 8U);
    }
    return value;
  };
  require(packed32(0) == packed.size() &&
              packed32(12) == metadata->catalog_id &&
              packed32(20) == 1'180'269'296U && packed[28] == std::byte{0x54} &&
              packed32(60) == 501 && packed32(64) == 2 && packed64(68) == 8 &&
              packed64(76) == 4 && packed64(84) == 4,
          "Darwin 8 getattrlist packing order or values are incorrect");
  const auto permanent = hfs::MetadataProvider::pack_attributes(
      *metadata, hfs::AttributeRequest{common_permanent_id, 0, 0, 0, 0});
  std::uint32_t packed_permanent = 0;
  for (std::size_t byte = 0; byte < 4; ++byte) {
    packed_permanent |= std::to_integer<std::uint32_t>(permanent[4 + byte])
                        << (byte * 8U);
  }
  require(packed_permanent == metadata->permanent_id,
          "ATTR_CMN_OBJPERMANENTID returned the catalog-link identity");

  AddressSpace memory;
  constexpr std::uint32_t guest_page = 0x4c000;
  require(memory.map(guest_page, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "HFS syscall test memory map failed");
  const auto copy_string = [&](std::uint32_t address, std::string_view value) {
    std::vector<std::byte> bytes(value.size() + 1U);
    std::transform(
        value.begin(), value.end(), bytes.begin(),
        [](char character) { return static_cast<std::byte>(character); });
    return memory.copy_in(address, bytes);
  };
  constexpr std::uint32_t path_address = guest_page;
  constexpr std::uint32_t attrlist_address = guest_page + 0x100;
  constexpr std::uint32_t output_address = guest_page + 0x200;
  require(copy_string(path_address, "/bin/demo") &&
              memory.write16(attrlist_address, 5) &&
              memory.write16(attrlist_address + 2, 0) &&
              memory.write32(attrlist_address + 4, common_object_id |
                                                       common_finder_info |
                                                       common_owner_id) &&
              memory.write32(attrlist_address + 8, 0) &&
              memory.write32(attrlist_address + 12, 0) &&
              memory.write32(attrlist_address + 16, file_resource_length) &&
              memory.write32(attrlist_address + 20, 0),
          "HFS getattrlist syscall fixture write failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream output_stream;
  Output output{output_stream};
  CompatibilityKernel kernel{memory, output, root};
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = attrlist_address;
  cpu.registers()[2] = output_address;
  cpu.registers()[3] = 256;
  cpu.registers()[4] = 0;
  cpu.registers()[12] = 220;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read32(output_address + 4) ==
                  std::optional<std::uint32_t>{metadata->catalog_id} &&
              memory.read8(output_address + 12) ==
                  std::optional<std::uint8_t>{0x54} &&
              memory.read32(output_address + 44) ==
                  std::optional<std::uint32_t>{501} &&
              memory.read64(output_address + 48) ==
                  std::optional<std::uint64_t>{4},
          "getattrlist syscall did not expose projected HFS metadata");

  constexpr auto volume_mask = volume_info | volume_signature | volume_name |
                               volume_capabilities | volume_attributes;
  require(copy_string(path_address, "/") &&
              memory.write32(attrlist_address + 4, 0) &&
              memory.write32(attrlist_address + 8, volume_mask) &&
              memory.write32(attrlist_address + 12, 0) &&
              memory.write32(attrlist_address + 16, 0) &&
              memory.write32(attrlist_address + 20, 0),
          "HFS volume getattrlist fixture write failed");
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = attrlist_address;
  cpu.registers()[2] = output_address;
  cpu.registers()[3] = 256;
  cpu.registers()[4] = 0;
  cpu.registers()[12] = 220;
  kernel.dispatch(cpu, 0x80);
  const auto volume_name_offset = memory.read32(output_address + 8);
  const auto volume_name_length = memory.read32(output_address + 12);
  require(cpu.registers()[0] == 0 &&
              memory.read32(output_address) ==
                  std::optional<std::uint32_t>{116} &&
              memory.read32(output_address + 4) ==
                  std::optional<std::uint32_t>{0x4858} &&
              memory.read32(output_address + 16) ==
                  std::optional<std::uint32_t>{0x00000f0fU} &&
              memory.read32(output_address + 20) ==
                  std::optional<std::uint32_t>{0x000003dfU} &&
              memory.read32(output_address + 48) ==
                  std::optional<std::uint32_t>{0x003fffffU} &&
              volume_name_offset == std::optional<std::uint32_t>{80} &&
              volume_name_length == std::optional<std::uint32_t>{26} &&
              memory.read_c_string(output_address + 8 +
                                   volume_name_offset.value_or(0)) ==
                  std::optional<std::string>{"Heavenly1A543a.UserBundle"},
          "root getattrlist did not return the iPhone HFSX volume view");

  require(copy_string(path_address, "/bin/demo"),
          "non-root volume getattrlist path fixture write failed");
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = attrlist_address;
  cpu.registers()[2] = output_address;
  cpu.registers()[3] = 256;
  cpu.registers()[4] = 0;
  cpu.registers()[12] = 220;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 22,
          "volume attributes were accepted on a non-root vnode");

  require(copy_string(path_address, "/bin/demo") &&
              memory.write32(attrlist_address + 4, common_object_id |
                                                       common_finder_info |
                                                       common_owner_id) &&
              memory.write32(attrlist_address + 8, 0) &&
              memory.write32(attrlist_address + 16, file_resource_length),
          "HFS file getattrlist fixture restoration failed");

  constexpr std::uint32_t stat_address = guest_page + 0x400;
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = stat_address;
  cpu.registers()[12] = 188;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read32(stat_address + 4) ==
                  std::optional<std::uint32_t>{metadata->permanent_id} &&
              memory.read16(stat_address + 8) ==
                  std::optional<std::uint16_t>{0100755U} &&
              memory.read32(stat_address + 12) ==
                  std::optional<std::uint32_t>{501},
          "stat syscall did not use the HFS vnode metadata projection");

  const auto host_permissions_before =
      std::filesystem::status(file).permissions();
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = 0640;
  cpu.registers()[12] = darwin::syscall::change_mode;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = 0x20;
  cpu.registers()[12] = darwin::syscall::change_flags;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = std::numeric_limits<std::uint32_t>::max();
  cpu.registers()[2] = 80;
  cpu.registers()[12] = darwin::syscall::change_owner;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = stat_address;
  cpu.registers()[12] = 188;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read16(stat_address + 8) ==
                  std::optional<std::uint16_t>{0100640U} &&
              memory.read32(stat_address + 12) ==
                  std::optional<std::uint32_t>{501} &&
              memory.read32(stat_address + 16) ==
                  std::optional<std::uint32_t>{80} &&
              memory.read32(stat_address + 68) ==
                  std::optional<std::uint32_t>{0x20} &&
              std::filesystem::status(file).permissions() ==
                  host_permissions_before,
          "HFS metadata mutation changed the host or was not observable");
  require(hfs::MetadataProvider{root}.query(file, true)->group == 501,
          "guest chown changed the extracted host metadata");

  cpu.registers()[0] = path_address;
  cpu.registers()[1] = darwin::open_flag::read_only;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = 5;
  kernel.dispatch(cpu, 0x80);
  const auto ownership_fd = cpu.registers()[0];
  cpu.registers()[0] = ownership_fd;
  cpu.registers()[1] = std::numeric_limits<std::uint32_t>::max();
  cpu.registers()[2] = 81;
  cpu.registers()[12] = darwin::syscall::change_owner_fd;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = stat_address;
  cpu.registers()[12] = 188;
  kernel.dispatch(cpu, 0x80);
  require(ownership_fd >= 3 && cpu.registers()[0] == 0 &&
              memory.read32(stat_address + 16) ==
                  std::optional<std::uint32_t>{81},
          "fchown did not update the shared HFS metadata overlay");
  cpu.registers()[0] = ownership_fd;
  cpu.registers()[1] = std::numeric_limits<std::uint32_t>::max();
  cpu.registers()[2] = 80;
  cpu.registers()[12] = darwin::syscall::change_owner_fd;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = ownership_fd;
  cpu.registers()[1] = 0604;
  cpu.registers()[12] = darwin::syscall::change_mode_fd;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = stat_address;
  cpu.registers()[12] = 188;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 && memory.read16(stat_address + 8) ==
                                         std::optional<std::uint16_t>{0100604U},
          "fchmod did not update the shared HFS metadata overlay");

  const auto setattr_common = common_modification_time | common_finder_info |
                              common_owner_id | common_access_mask |
                              common_flags;
  require(memory.write32(attrlist_address + 4, setattr_common) &&
              memory.write32(attrlist_address + 8, 0) &&
              memory.write32(attrlist_address + 12, 0) &&
              memory.write32(attrlist_address + 16, 0) &&
              memory.write32(attrlist_address + 20, 0),
          "HFS setattrlist mask fixture write failed");
  std::array<std::byte, 52> set_attributes{};
  const auto set_attribute_word = [&](std::size_t offset, std::uint32_t value) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      set_attributes[offset + byte] =
          static_cast<std::byte>(value >> (byte * 8U));
    }
  };
  set_attribute_word(0, 1'111);
  set_attribute_word(4, 222);
  set_attributes[8] = std::byte{0xaa};
  set_attribute_word(40, 502);
  set_attribute_word(44, 0601);
  set_attribute_word(48, 0x40);
  require(memory.copy_in(output_address, set_attributes),
          "HFS setattrlist value fixture write failed");
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = attrlist_address;
  cpu.registers()[2] = output_address;
  cpu.registers()[3] = set_attributes.size();
  cpu.registers()[4] = 0;
  cpu.registers()[12] = 221;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = stat_address;
  cpu.registers()[12] = 188;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read16(stat_address + 8) ==
                  std::optional<std::uint16_t>{0100601U} &&
              memory.read32(stat_address + 12) ==
                  std::optional<std::uint32_t>{502} &&
              memory.read32(stat_address + 32) ==
                  std::optional<std::uint32_t>{1'111} &&
              memory.read32(stat_address + 36) ==
                  std::optional<std::uint32_t>{222} &&
              memory.read32(stat_address + 68) ==
                  std::optional<std::uint32_t>{0x40},
          "setattrlist did not update the HFS metadata overlay");
  require(memory.write32(attrlist_address + 4, common_finder_info),
          "HFS FinderInfo get mask write failed");
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = attrlist_address;
  cpu.registers()[2] = output_address;
  cpu.registers()[3] = 64;
  cpu.registers()[4] = 0;
  cpu.registers()[12] = 220;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 && memory.read8(output_address + 4) ==
                                         std::optional<std::uint8_t>{0xaa},
          "getattrlist did not observe setattrlist FinderInfo");

  AddressSpace child_memory;
  const auto child_path_bytes = memory.read_bytes(
      path_address, std::string_view{"/bin/demo"}.size() + 1U);
  require(child_memory.map(guest_page, AddressSpace::page_size,
                           MemoryPermission::Read | MemoryPermission::Write) &&
              child_path_bytes &&
              child_memory.copy_in(path_address, *child_path_bytes),
          "HFS child metadata test memory setup failed");
  Dynarmic::ExclusiveMonitor child_monitor{1};
  Cpu child_cpu{0, child_memory, child_monitor};
  CompatibilityKernel child_kernel{child_memory, output, root};
  child_kernel.inherit_process_state(kernel, 42);
  child_cpu.registers()[0] = path_address;
  child_cpu.registers()[1] = stat_address;
  child_cpu.registers()[12] = 188;
  child_kernel.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == 0 &&
              child_memory.read16(stat_address + 8) ==
                  std::optional<std::uint16_t>{0100601U} &&
              child_memory.read32(stat_address + 16) ==
                  std::optional<std::uint32_t>{80} &&
              child_memory.read32(stat_address + 68) ==
                  std::optional<std::uint32_t>{0x40},
          "HFS metadata overlay was not shared across guest processes");

  constexpr std::uint32_t resource_path_address = guest_page + 0x600;
  constexpr std::uint32_t resource_output_address = guest_page + 0x700;
  require(copy_string(resource_path_address, "/bin/demo/..namedfork/rsrc"),
          "HFS named-fork path fixture write failed");
  cpu.registers()[0] = resource_path_address;
  cpu.registers()[1] = darwin::open_flag::read_only;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = 5;
  kernel.dispatch(cpu, 0x80);
  const auto resource_fd = cpu.registers()[0];
  cpu.registers()[0] = resource_fd;
  cpu.registers()[1] = resource_output_address;
  cpu.registers()[2] = 16;
  cpu.registers()[12] = darwin::syscall::read;
  kernel.dispatch(cpu, 0x80);
  require(resource_fd >= 3 && cpu.registers()[0] == 4 &&
              memory.read_bytes(resource_output_address, 4) ==
                  std::optional{
                      std::vector<std::byte>{std::byte{'R'}, std::byte{'S'},
                                             std::byte{'R'}, std::byte{'C'}}},
          "HFS named resource fork did not map to its hidden backing data");

  require(copy_string(resource_path_address, "com.example.test") &&
              memory.copy_in(resource_output_address,
                             std::array{std::byte{'M'}, std::byte{'E'},
                                        std::byte{'T'}, std::byte{'A'}}),
          "HFS named-xattr set fixture write failed");
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = resource_path_address;
  cpu.registers()[2] = resource_output_address;
  cpu.registers()[3] = 4;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0x2; // XATTR_CREATE
  cpu.registers()[12] = 236;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              copy_string(path_address, "/bin/demo-link"),
          "Darwin setxattr did not create an HFS named attribute");
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = resource_path_address;
  cpu.registers()[2] = resource_output_address;
  cpu.registers()[3] = 4;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 234;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 4 &&
              memory.read_bytes(resource_output_address, 4) ==
                  std::optional{
                      std::vector<std::byte>{std::byte{'M'}, std::byte{'E'},
                                             std::byte{'T'}, std::byte{'A'}}},
          "HFS named attributes were not shared by hard-link inode ID");

  require(copy_string(path_address, "/bin/demo") &&
              copy_string(resource_path_address, "com.apple.ResourceFork") &&
              memory.copy_in(resource_output_address,
                             std::array{std::byte{'X'}, std::byte{'Y'}}),
          "HFS ResourceFork xattr fixture write failed");
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = resource_path_address;
  cpu.registers()[2] = resource_output_address;
  cpu.registers()[3] = 2;
  cpu.registers()[4] = 2;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 236;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = resource_path_address;
  cpu.registers()[2] = resource_output_address;
  cpu.registers()[3] = 2;
  cpu.registers()[4] = 1;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 234;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 2 &&
              memory.read_bytes(resource_output_address, 2) ==
                  std::optional{
                      std::vector<std::byte>{std::byte{'S'}, std::byte{'X'}}},
          "ResourceFork xattr position did not update the named fork");

  cpu.registers()[0] = path_address;
  cpu.registers()[1] = resource_output_address;
  cpu.registers()[2] = 256;
  cpu.registers()[3] = 0;
  cpu.registers()[12] = 240;
  kernel.dispatch(cpu, 0x80);
  const auto xattr_list =
      memory.read_bytes(resource_output_address, cpu.registers()[0]);
  const auto contains_xattr_name = [&](std::string_view name) {
    return xattr_list &&
           std::search(xattr_list->begin(), xattr_list->end(), name.begin(),
                       name.end(), [](std::byte byte, char character) {
                         return std::to_integer<unsigned char>(byte) ==
                                static_cast<unsigned char>(character);
                       }) != xattr_list->end();
  };
  require(contains_xattr_name("com.apple.FinderInfo") &&
              contains_xattr_name("com.apple.ResourceFork") &&
              contains_xattr_name("com.example.test"),
          "listxattr omitted projected or guest-created HFS attributes");

  require(copy_string(resource_path_address, "com.example.test"),
          "HFS removexattr name fixture write failed");
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = resource_path_address;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = 238;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = path_address;
  cpu.registers()[1] = resource_path_address;
  cpu.registers()[2] = 0;
  cpu.registers()[3] = 0;
  cpu.registers()[4] = 0;
  cpu.registers()[5] = 0;
  cpu.registers()[12] = 234;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 93,
          "removexattr did not leave an ENOATTR overlay tombstone");

  constexpr std::uint32_t directory_path_address = guest_page + 0x800;
  constexpr std::uint32_t directory_output_address = guest_page + 0x900;
  require(copy_string(directory_path_address, "/bin"),
          "HFS directory path fixture write failed");
  cpu.registers()[0] = directory_path_address;
  cpu.registers()[1] = darwin::open_flag::read_only;
  cpu.registers()[12] = 5;
  kernel.dispatch(cpu, 0x80);
  const auto directory_fd = cpu.registers()[0];
  cpu.registers()[0] = directory_fd;
  cpu.registers()[1] = directory_output_address;
  cpu.registers()[2] = 512;
  cpu.registers()[3] = 0;
  cpu.registers()[12] = 196;
  kernel.dispatch(cpu, 0x80);
  const auto directory_size = cpu.registers()[0];
  const auto directory_bytes =
      memory.read_bytes(directory_output_address, directory_size);
  require(directory_fd >= 3 && directory_bytes &&
              std::search(directory_bytes->begin(), directory_bytes->end(),
                          hfs::resource_sidecar_suffix.begin(),
                          hfs::resource_sidecar_suffix.end(),
                          [](std::byte byte, char character) {
                            return std::to_integer<unsigned char>(byte) ==
                                   static_cast<unsigned char>(character);
                          }) == directory_bytes->end(),
          "HFS resource sidecar leaked through guest directory enumeration");

  constexpr std::uint32_t created_path_address = guest_page + 0xb00;
  constexpr std::uint32_t created_stat_address = guest_page + 0xc00;
  cpu.registers()[0] = 0027;
  cpu.registers()[12] = 60;
  kernel.dispatch(cpu, 0x80);
  require(copy_string(created_path_address, "/bin/newdir"),
          "HFS created-directory path fixture write failed");
  cpu.registers()[0] = created_path_address;
  cpu.registers()[1] = 0777;
  cpu.registers()[12] = 136;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = created_path_address;
  cpu.registers()[1] = created_stat_address;
  cpu.registers()[12] = 188;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 && memory.read16(created_stat_address + 8) ==
                                         std::optional<std::uint16_t>{0040750U},
          "HFS directory creation did not apply the guest umask overlay");

  require(copy_string(created_path_address, "/bin/new-file"),
          "HFS created-file path fixture write failed");
  cpu.registers()[0] = created_path_address;
  cpu.registers()[1] =
      darwin::open_flag::create | darwin::open_flag::write_only;
  cpu.registers()[2] = 0666;
  cpu.registers()[12] = 5;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] >= 3,
          "HFS created-file open did not return a descriptor");
  cpu.registers()[0] = created_path_address;
  cpu.registers()[1] = created_stat_address;
  cpu.registers()[12] = 188;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 && memory.read16(created_stat_address + 8) ==
                                         std::optional<std::uint16_t>{0100640U},
          "HFS file creation did not apply the guest umask overlay");

  std::filesystem::remove_all(test_directory, filesystem_error);
}

void hfs_vfs_mutation_test() {
  const auto test_directory =
      std::filesystem::temp_directory_path() / "ilemu-hfs-vfs-tests";
  const auto root = test_directory / "rootfs";
  std::error_code filesystem_error;
  std::filesystem::remove_all(test_directory, filesystem_error);
  std::filesystem::create_directories(root / "data/real", filesystem_error);
  require(!filesystem_error, "HFS VFS test root creation failed");
  {
    std::ofstream stream{root / "data/source", std::ios::binary};
    stream << "DATA";
  }
  {
    std::ofstream stream{
        hfs::MetadataProvider::resource_sidecar(root / "data/source"),
        std::ios::binary};
    stream << "RSRC";
  }
  {
    std::ofstream stream{root / "data/real/file", std::ios::binary};
    stream << "SAFE";
  }

  AddressSpace memory;
  constexpr std::uint32_t guest_page = 0x4e000;
  require(memory.map(guest_page, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "HFS VFS guest memory map failed");
  const auto copy_string = [&](std::uint32_t address, std::string_view value) {
    std::vector<std::byte> bytes(value.size() + 1U);
    std::transform(
        value.begin(), value.end(), bytes.begin(),
        [](char character) { return static_cast<std::byte>(character); });
    return memory.copy_in(address, bytes);
  };
  constexpr std::uint32_t first_path = guest_page;
  constexpr std::uint32_t second_path = guest_page + 0x100;
  constexpr std::uint32_t output = guest_page + 0x300;
  constexpr std::uint32_t stat_output = guest_page + 0x500;
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream output_stream;
  Output kernel_output{output_stream};
  CompatibilityKernel kernel{memory, kernel_output, root};

  require(copy_string(first_path, "/data/source") &&
              copy_string(second_path, "/data/hard"),
          "HFS hard-link path fixture write failed");
  cpu.registers()[0] = first_path;
  cpu.registers()[1] = second_path;
  cpu.registers()[12] = 9;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              std::filesystem::equivalent(root / "data/source",
                                          root / "data/hard") &&
              std::filesystem::equivalent(
                  hfs::MetadataProvider::resource_sidecar(root / "data/source"),
                  hfs::MetadataProvider::resource_sidecar(root / "data/hard")),
          "link syscall did not preserve data/resource-fork identity");

  cpu.registers()[0] = second_path;
  cpu.registers()[1] = 0600;
  cpu.registers()[12] = 15;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = first_path;
  cpu.registers()[1] = stat_output;
  cpu.registers()[12] = 188;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read16(stat_output + 8) ==
                  std::optional<std::uint16_t>{0100600U} &&
              memory.read16(stat_output + 10) ==
                  std::optional<std::uint16_t>{2},
          "HFS hard-link metadata overlay was not inode-shared");

  require(copy_string(first_path, "/data/source") &&
              copy_string(second_path, "/data/sym"),
          "HFS symlink path fixture write failed");
  cpu.registers()[0] = first_path;
  cpu.registers()[1] = second_path;
  cpu.registers()[12] = 57;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = second_path;
  cpu.registers()[1] = output;
  cpu.registers()[2] = 128;
  cpu.registers()[12] = 58;
  kernel.dispatch(cpu, 0x80);
  const auto link_size = cpu.registers()[0];
  const auto link_bytes = memory.read_bytes(output, link_size);
  require(link_bytes &&
              std::string{reinterpret_cast<const char *>(link_bytes->data()),
                          link_bytes->size()} == "/data/source",
          "readlink did not return the original guest symlink target");
  cpu.registers()[0] = second_path;
  cpu.registers()[1] = stat_output;
  cpu.registers()[12] = 190;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              (memory.read16(stat_output + 8).value_or(0) & 0170000U) ==
                  0120000U,
          "lstat did not preserve the guest symlink vnode type");

  require(copy_string(first_path, "/data/hard") &&
              copy_string(second_path, "/data/renamed"),
          "HFS rename path fixture write failed");
  cpu.registers()[0] = first_path;
  cpu.registers()[1] = second_path;
  cpu.registers()[12] = 128;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              !std::filesystem::exists(root / "data/hard") &&
              std::filesystem::exists(root / "data/renamed") &&
              std::filesystem::exists(hfs::MetadataProvider::resource_sidecar(
                  root / "data/renamed")),
          "rename did not move the HFS data/resource fork pair");
  cpu.registers()[0] = second_path;
  cpu.registers()[1] = stat_output;
  cpu.registers()[12] = 188;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 && memory.read16(stat_output + 8) ==
                                         std::optional<std::uint16_t>{0100600U},
          "rename lost the permanent-ID metadata overlay");

  cpu.registers()[0] = second_path;
  cpu.registers()[12] = 10;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              !std::filesystem::exists(root / "data/renamed") &&
              !std::filesystem::exists(hfs::MetadataProvider::resource_sidecar(
                  root / "data/renamed")) &&
              std::filesystem::exists(root / "data/source"),
          "unlink removed the wrong hard link or leaked its resource fork");
  cpu.registers()[0] = first_path;
  require(copy_string(first_path, "/data/source"),
          "HFS surviving hard-link path fixture write failed");
  cpu.registers()[1] = stat_output;
  cpu.registers()[12] = 188;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read16(stat_output + 8) ==
                  std::optional<std::uint16_t>{0100600U} &&
              memory.read16(stat_output + 10) ==
                  std::optional<std::uint16_t>{1},
          "unlink discarded metadata still owned by a surviving hard link");

  require(copy_string(first_path, "/data/real") &&
              copy_string(second_path, "/data/dirlink"),
          "HFS intermediate symlink path fixture write failed");
  cpu.registers()[0] = first_path;
  cpu.registers()[1] = second_path;
  cpu.registers()[12] = 57;
  kernel.dispatch(cpu, 0x80);
  require(copy_string(first_path, "/data/dirlink/file"),
          "HFS intermediate symlink open path fixture write failed");
  cpu.registers()[0] = first_path;
  cpu.registers()[1] = darwin::open_flag::read_only;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = 5;
  kernel.dispatch(cpu, 0x80);
  const auto safe_fd = cpu.registers()[0];
  cpu.registers()[0] = safe_fd;
  cpu.registers()[1] = output;
  cpu.registers()[2] = 4;
  cpu.registers()[12] = darwin::syscall::read;
  kernel.dispatch(cpu, 0x80);
  require(safe_fd >= 3 && cpu.registers()[0] == 4 &&
              memory.read_bytes(output, 4) ==
                  std::optional{
                      std::vector<std::byte>{std::byte{'S'}, std::byte{'A'},
                                             std::byte{'F'}, std::byte{'E'}}},
          "component-wise guest symlink resolution escaped or missed rootfs");

  require(copy_string(first_path, "/data/empty"),
          "HFS rmdir path fixture write failed");
  cpu.registers()[0] = first_path;
  cpu.registers()[1] = 0755;
  cpu.registers()[12] = 136;
  kernel.dispatch(cpu, 0x80);
  cpu.registers()[0] = first_path;
  cpu.registers()[12] = 137;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              !std::filesystem::exists(root / "data/empty"),
          "rmdir did not remove the host-backed guest directory");

  std::filesystem::remove_all(test_directory, filesystem_error);
}

void advisory_file_lock_test() {
  const auto test_directory =
      std::filesystem::temp_directory_path() / "ilemu-flock-tests";
  const auto root = test_directory / "rootfs";
  std::error_code filesystem_error;
  std::filesystem::remove_all(test_directory, filesystem_error);
  std::filesystem::create_directories(root / "var/run", filesystem_error);
  require(!filesystem_error, "flock test root creation failed");
  {
    std::ofstream stream{root / "var/run/preferences.lock", std::ios::binary};
    stream << "lock";
  }

  constexpr std::uint32_t guest_page = 0x5e000;
  constexpr std::uint32_t path_address = guest_page;
  const auto prepare_memory = [](AddressSpace &memory) {
    constexpr std::string_view path{"/var/run/preferences.lock"};
    std::vector<std::byte> bytes(path.size() + 1U);
    std::transform(path.begin(), path.end(), bytes.begin(),
                   [](char value) { return static_cast<std::byte>(value); });
    return memory.map(guest_page, AddressSpace::page_size,
                      MemoryPermission::Read | MemoryPermission::Write) &&
           memory.copy_in(path_address, bytes);
  };

  AddressSpace parent_memory;
  require(prepare_memory(parent_memory), "flock parent memory setup failed");
  Dynarmic::ExclusiveMonitor parent_monitor{1};
  Cpu parent_cpu{0, parent_memory, parent_monitor};
  std::ostringstream output_stream;
  Output output{output_stream};
  CompatibilityKernel parent{parent_memory, output, root};
  parent_cpu.registers()[0] = path_address;
  parent_cpu.registers()[1] = darwin::open_flag::read_write;
  parent_cpu.registers()[2] = 0;
  parent_cpu.registers()[12] = 5;
  parent.dispatch(parent_cpu, 0x80);
  const auto inherited_fd = parent_cpu.registers()[0];
  require(inherited_fd >= 3, "flock parent open failed");

  AddressSpace child_memory;
  require(prepare_memory(child_memory), "flock child memory setup failed");
  Dynarmic::ExclusiveMonitor child_monitor{1};
  Cpu child_cpu{0, child_memory, child_monitor};
  CompatibilityKernel child{child_memory, output, root};
  child.inherit_process_state(parent, 42);

  const auto flock = [](CompatibilityKernel &kernel, Cpu &cpu, std::uint32_t fd,
                        std::uint32_t operation) {
    cpu.registers()[0] = fd;
    cpu.registers()[1] = operation;
    cpu.registers()[12] = darwin::syscall::flock;
    kernel.dispatch(cpu, 0x80);
  };
  flock(parent, parent_cpu, inherited_fd, darwin::flock_operation::exclusive);
  flock(child, child_cpu, inherited_fd,
        darwin::flock_operation::shared |
            darwin::flock_operation::non_blocking);
  require(parent_cpu.registers()[0] == 0 && child_cpu.registers()[0] == 0,
          "fork did not retain the same flock open-description owner");
  flock(parent, parent_cpu, inherited_fd, darwin::flock_operation::exclusive);

  child_cpu.registers()[0] = path_address;
  child_cpu.registers()[1] = darwin::open_flag::read_write;
  child_cpu.registers()[2] = 0;
  child_cpu.registers()[12] = 5;
  child.dispatch(child_cpu, 0x80);
  const auto independent_fd = child_cpu.registers()[0];
  require(independent_fd != inherited_fd && independent_fd >= 3,
          "second open did not allocate an independent descriptor");

  flock(child, child_cpu, independent_fd,
        darwin::flock_operation::shared |
            darwin::flock_operation::non_blocking);
  require(child_cpu.registers()[0] == darwin::error::would_block &&
              (child_cpu.cpsr() & (1U << 29U)) != 0,
          "nonblocking flock conflict did not return EWOULDBLOCK");
  flock(child, child_cpu, independent_fd, darwin::flock_operation::shared);
  require(child.wait_reason(child_cpu.processor_id()) ==
              "flock(fd=" + std::to_string(independent_fd) + ")",
          "blocking flock did not suspend the guest thread");
  flock(parent, parent_cpu, inherited_fd, darwin::flock_operation::unlock);
  require(child.deliver_pending_io(child_cpu) &&
              child_cpu.registers()[0] == 0 &&
              (child_cpu.cpsr() & (1U << 29U)) == 0,
          "flock waiter did not wake after unlock");

  flock(parent, parent_cpu, inherited_fd,
        darwin::flock_operation::exclusive |
            darwin::flock_operation::non_blocking);
  require(parent_cpu.registers()[0] == darwin::error::would_block,
          "independent shared flock did not exclude a writer");
  child_cpu.registers()[0] = independent_fd;
  child_cpu.registers()[12] = darwin::syscall::close;
  child.dispatch(child_cpu, 0x80);
  flock(parent, parent_cpu, inherited_fd,
        darwin::flock_operation::exclusive |
            darwin::flock_operation::non_blocking);
  require(parent_cpu.registers()[0] == 0,
          "final close did not release the open description's flock");

  std::filesystem::remove_all(test_directory, filesystem_error);
}

void posix_record_lock_test() {
  const auto test_directory =
      std::filesystem::temp_directory_path() / "ilemu-fcntl-lock-tests";
  const auto root = test_directory / "rootfs";
  std::error_code filesystem_error;
  std::filesystem::remove_all(test_directory, filesystem_error);
  std::filesystem::create_directories(root / "var/root/Library/Calendar",
                                      filesystem_error);
  require(!filesystem_error, "fcntl lock test root creation failed");
  {
    std::ofstream stream{root / "var/root/Library/Calendar/Calendar.sqlitedb",
                         std::ios::binary};
    stream << "SQLite format 3";
  }

  constexpr std::uint32_t guest_page = 0x61000;
  constexpr std::uint32_t path_address = guest_page;
  constexpr std::uint32_t flock_address = guest_page + 0x100;
  const auto prepare_memory = [](AddressSpace &memory) {
    constexpr std::string_view path{
        "/var/root/Library/Calendar/Calendar.sqlitedb"};
    std::vector<std::byte> bytes(path.size() + 1U);
    std::transform(path.begin(), path.end(), bytes.begin(),
                   [](char value) { return static_cast<std::byte>(value); });
    return memory.map(guest_page, AddressSpace::page_size,
                      MemoryPermission::Read | MemoryPermission::Write) &&
           memory.copy_in(path_address, bytes);
  };
  const auto write_flock = [](AddressSpace &memory, std::uint64_t start,
                              std::uint64_t length, std::uint16_t type) {
    return memory.write64(flock_address + darwin::record_lock::start_offset,
                          start) &&
           memory.write64(flock_address + darwin::record_lock::length_offset,
                          length) &&
           memory.write32(flock_address + darwin::record_lock::pid_offset, 0) &&
           memory.write16(flock_address + darwin::record_lock::type_offset,
                          type) &&
           memory.write16(flock_address + darwin::record_lock::whence_offset,
                          0);
  };
  const auto fcntl_lock = [](CompatibilityKernel &kernel, Cpu &cpu,
                             std::uint32_t fd, std::uint32_t command) {
    cpu.registers()[0] = fd;
    cpu.registers()[1] = command;
    cpu.registers()[2] = flock_address;
    cpu.registers()[12] = darwin::syscall::fcntl;
    kernel.dispatch(cpu, 0x80);
  };

  AddressSpace parent_memory;
  AddressSpace child_memory;
  require(prepare_memory(parent_memory) && prepare_memory(child_memory),
          "fcntl lock guest memory setup failed");
  Dynarmic::ExclusiveMonitor parent_monitor{1};
  Dynarmic::ExclusiveMonitor child_monitor{1};
  Cpu parent_cpu{0, parent_memory, parent_monitor};
  Cpu child_cpu{0, child_memory, child_monitor};
  std::ostringstream output_stream;
  Output output{output_stream};
  CompatibilityKernel parent{parent_memory, output, root};
  CompatibilityKernel child{child_memory, output, root};

  parent_cpu.registers()[0] = path_address;
  parent_cpu.registers()[1] = darwin::open_flag::read_write;
  parent_cpu.registers()[2] = 0;
  parent_cpu.registers()[12] = 5;
  parent.dispatch(parent_cpu, 0x80);
  const auto fd = parent_cpu.registers()[0];
  require(fd >= 3, "fcntl lock database open failed");
  child.inherit_process_state(parent, 42);

  // SQLite 3.1.3 probes the 1 GiB PENDING_BYTE with F_SETLK. Returning
  // EINVAL here becomes SQLITE_NOLFS and prevents SpringBoard startup.
  constexpr std::uint64_t sqlite_pending_byte = 0x40000000ULL;
  require(write_flock(parent_memory, sqlite_pending_byte, 1,
                      darwin::record_lock::write),
          "fcntl lock request write failed");
  fcntl_lock(parent, parent_cpu, fd, darwin::fcntl_command::set_record_lock);
  require(parent_cpu.registers()[0] == 0 &&
              (parent_cpu.cpsr() & (1U << 29U)) == 0,
          "SQLite pending-byte F_SETLK was rejected");

  require(write_flock(child_memory, sqlite_pending_byte, 1,
                      darwin::record_lock::write),
          "fcntl get-lock request write failed");
  fcntl_lock(child, child_cpu, fd, darwin::fcntl_command::get_record_lock);
  require(child_cpu.registers()[0] == 0 &&
              child_memory.read16(flock_address +
                                  darwin::record_lock::type_offset) ==
                  darwin::record_lock::write &&
              child_memory.read32(flock_address +
                                  darwin::record_lock::pid_offset) == 1,
          "F_GETLK did not report the conflicting process lock");

  require(write_flock(child_memory, sqlite_pending_byte, 1,
                      darwin::record_lock::write),
          "fcntl conflict request write failed");
  fcntl_lock(child, child_cpu, fd, darwin::fcntl_command::set_record_lock);
  require(child_cpu.registers()[0] == darwin::error::would_block &&
              (child_cpu.cpsr() & (1U << 29U)) != 0,
          "conflicting F_SETLK did not return EWOULDBLOCK");

  fcntl_lock(child, child_cpu, fd, darwin::fcntl_command::set_record_lock_wait);
  require(child.wait_reason(child_cpu.processor_id()) ==
              "fcntl-lock(fd=" + std::to_string(fd) + ")",
          "F_SETLKW did not suspend on the conflicting range");
  require(write_flock(parent_memory, sqlite_pending_byte, 1,
                      darwin::record_lock::unlock),
          "fcntl unlock request write failed");
  fcntl_lock(parent, parent_cpu, fd, darwin::fcntl_command::set_record_lock);
  require(child.deliver_pending_io(child_cpu) &&
              child_cpu.registers()[0] == 0 &&
              (child_cpu.cpsr() & (1U << 29U)) == 0,
          "F_SETLKW waiter did not wake after unlock");

  child_cpu.registers()[0] = fd;
  child_cpu.registers()[12] = darwin::syscall::close;
  child.dispatch(child_cpu, 0x80);
  require(write_flock(parent_memory, sqlite_pending_byte, 1,
                      darwin::record_lock::write),
          "fcntl close-release request write failed");
  fcntl_lock(parent, parent_cpu, fd, darwin::fcntl_command::set_record_lock);
  require(parent_cpu.registers()[0] == 0,
          "closing an fd did not release the process record lock");

  std::filesystem::remove_all(test_directory, filesystem_error);
}

void run_tests() {
  filesystem_directory_syscall_test();
  writable_file_syscall_test();
  hfs_metadata_projection_test();
  hfs_vfs_mutation_test();
  advisory_file_lock_test();
  posix_record_lock_test();
}

} // namespace

int main() { return ilemu::test::run_suite("filesystem", run_tests); }
