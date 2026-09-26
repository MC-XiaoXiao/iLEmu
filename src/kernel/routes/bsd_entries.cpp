// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../bsd/process/resource_monitor.hpp"
#include "../bsd/process/uuid_policy.hpp"
#include "catalog_internal.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/darwin_memorystatus_abi.hpp"
#include "kernel/darwin_proc_info_abi.hpp"
#include "kernel/darwin_process_policy_abi.hpp"
#include "kernel/darwin_resource_abi.hpp"
#include "kernel/kernel_bsd_interval_timer.hpp"
namespace ilemu::syscall_routes {
void bind_bsd_entries(Table& table, const DarwinAbi& abi)
{
    const auto add = [&](std::uint32_t n, std::string_view operation,
                         Handler handler,
                         Contract contract = Contract::CurrentDispatcher,
                         bool supported = true) {
        table.bind_new({ Domain::BsdSyscall, n, n, operation, handler, contract,
            Cancellation::OriginalEntry,
            supported ? Outcome::HandlerValidated : Outcome::BsdNosys,
            "bsd/dispatch.cpp" });
    };
    add(kernel_bsd::resource_monitor::ledger_route.identifier, "ledger",
        Handler::BsdLedgerInline, Contract::Ledger,
        darwin_abi_route_supported(
            kernel_bsd::resource_monitor::ledger_route, abi.abi_epoch));
    add(458, "coalition", Handler::BsdCoalition, Contract::ResourceCoalitions,
        abi.coalition_abi == DarwinCoalitionAbi::ResourceCoalitions);
    add(460, "necp_match_policy", Handler::BsdNetworkPolicy,
        Contract::LaterEpoch, abi.abi_epoch == DarwinAbiEpoch::Later);
    add(461, "getattrlistbulk", Handler::BsdDirectoryAttributes,
        Contract::LaterEpoch, abi.abi_epoch == DarwinAbiEpoch::Later);
    add(463, "openat", Handler::BsdFilesystem, Contract::LaterEpoch,
        abi.abi_epoch == DarwinAbiEpoch::Later);
    table.bind_new({ Domain::BsdSyscall, 464, 464, "openat_nocancel",
        Handler::BsdFilesystem, Contract::LaterEpoch,
        Cancellation::NoCancelEntry,
        abi.abi_epoch == DarwinAbiEpoch::Later ? Outcome::HandlerValidated
                                               : Outcome::BsdNosys,
        "bsd/dispatch.cpp" });
    add(467, "fchmodat", Handler::BsdFilesystem, Contract::LaterEpoch,
        abi.abi_epoch == DarwinAbiEpoch::Later);
    add(468, "fchownat", Handler::BsdFilesystem, Contract::LaterEpoch,
        abi.abi_epoch == DarwinAbiEpoch::Later);
    add(441, "guarded_open_np", Handler::BsdGuardedFile);
    add(442, "guarded_close_np", Handler::BsdGuardedFile);
    add(443, "guarded_kqueue_np", Handler::BsdGuardedFile);
    add(484, "guarded_open_dprotected_np", Handler::BsdGuardedFile,
        Contract::LaterEpoch, abi.abi_epoch == DarwinAbiEpoch::Later);
    add(485, "guarded_write_np", Handler::BsdGuardedFile, Contract::LaterEpoch,
        abi.abi_epoch == DarwinAbiEpoch::Later);
    add(486, "guarded_pwrite_np", Handler::BsdGuardedFile, Contract::LaterEpoch,
        abi.abi_epoch == DarwinAbiEpoch::Later);
    add(444, "change_fdguard_np", Handler::BsdGuardedFile,
        Contract::GuardedFdChange,
        darwin_abi_route_supported(guarded_fd_change_route, abi.abi_epoch));
    add(447, "connectx", Handler::BsdSocket, Contract::Connectx,
        darwin_abi_route_supported(connectx_route, abi.abi_epoch));
    add(322, "iopolicysys", Handler::BsdIoPolicyInline, Contract::IoPolicy,
        darwin_abi_route_supported(legacy_iopolicysys_route, abi.abi_epoch));
    add(365, "stack_snapshot", Handler::BsdStackSnapshotInline,
        Contract::StackSnapshot,
        abi.stack_snapshot_abi != DarwinStackSnapshotAbi::Unsupported);
    add(0, "syscall", Handler::BsdProcess);
    add(1, "exit", Handler::BsdProcess);
    add(2, "fork", Handler::BsdProcess);
    add(66, "vfork", Handler::BsdProcess);
    add(7, "wait4", Handler::BsdProcess);
    add(20, "getpid", Handler::BsdProcess);
    add(darwin::syscall::get_priority, "get_priority", Handler::BsdProcess);
    add(darwin::syscall::set_user_id, "set_user_id", Handler::BsdProcess);
    add(24, "getuid", Handler::BsdProcess);
    add(25, "geteuid", Handler::BsdProcess);
    add(39, "getppid", Handler::BsdProcess);
    add(43, "getegid", Handler::BsdProcess);
    add(47, "getgid", Handler::BsdProcess);
    add(46, "sigaction", Handler::BsdProcess);
    add(48, "sigprocmask", Handler::BsdProcess);
    add(darwin::syscall::pthread_sigmask, "pthread_sigmask",
        Handler::BsdProcess);
    add(49, "getlogin", Handler::BsdProcess);
    add(50, "setlogin", Handler::BsdProcess);
    add(55, "reboot", Handler::BsdProcess);
    add(60, "umask", Handler::BsdProcess);
    add(59, "execve", Handler::BsdProcess);
    add(darwin::syscall::get_process_group, "get_process_group",
        Handler::BsdProcess);
    add(darwin::syscall::get_thread_identity, "gettid", Handler::BsdProcess);
    add(kernel_bsd::interval_timer::set_syscall, "set_syscall",
        Handler::BsdProcess);
    add(kernel_bsd::interval_timer::get_syscall, "get_syscall",
        Handler::BsdProcess);
    add(244, "posix_spawn", Handler::BsdProcess);
    add(96, "setpriority", Handler::BsdProcess);
    add(116, "gettimeofday", Handler::BsdProcess);
    add(darwin::syscall::get_resource_usage, "get_resource_usage",
        Handler::BsdProcess);
    add(darwin::syscall::set_time_of_day, "set_time_of_day",
        Handler::BsdProcess);
    add(darwin::syscall::set_real_effective_user_id,
        "set_real_effective_user_id", Handler::BsdProcess);
    add(darwin::syscall::set_real_effective_group_id,
        "set_real_effective_group_id", Handler::BsdProcess);
    add(147, "setsid", Handler::BsdProcess);
    add(darwin::syscall::set_groups, "set_groups", Handler::BsdProcess);
    add(darwin::syscall::set_group_id, "set_group_id", Handler::BsdProcess);
    add(darwin::syscall::set_effective_group_id, "set_effective_group_id",
        Handler::BsdProcess);
    add(darwin::syscall::set_effective_user_id, "set_effective_user_id",
        Handler::BsdProcess);
    add(darwin::syscall::init_groups, "init_groups", Handler::BsdProcess);
    add(darwin::syscall::get_resource_limit, "get_resource_limit",
        Handler::BsdProcess);
    add(darwin::syscall::set_resource_limit, "set_resource_limit",
        Handler::BsdProcess);
    add(kernel_bsd::resource_monitor::syscall_number, "proc_rlimit_control",
        Handler::BsdProcess);
    add(kernel_bsd::uuid_policy::syscall_number, "proc_uuid_policy",
        Handler::BsdProcess);
    add(darwin::syscall::disable_thread_signal, "disable_thread_signal",
        Handler::BsdProcess);
    add(333, "__pthread_canceled", Handler::BsdProcess);
    add(darwin::syscall::semaphore_wait_signal, "semaphore_wait_signal",
        Handler::BsdProcess);
    add(darwin::syscall::semaphore_wait_signal_timespec,
        "semaphore_wait_signal_timespec", Handler::BsdProcess);
    add(darwin::proc_info::syscall_number, "proc_info", Handler::BsdProcess);
    add(darwin::memorystatus::syscall_number, "memorystatus_control",
        Handler::BsdProcess);
    add(darwin::process_policy::syscall_number, "process_policy",
        Handler::BsdProcess);
    add(327, "issetugid", Handler::BsdProcess);
    add(355, "getaudit", Handler::BsdProcess);
    add(darwin::syscall::pid_suspend, "pid_suspend", Handler::BsdProcess);
    add(darwin::syscall::pid_resume, "pid_resume", Handler::BsdProcess);
    add(darwin::syscall::pid_hibernate, "pid_hibernate", Handler::BsdProcess);
    add(darwin::syscall::pid_shutdown_sockets, "pid_shutdown_sockets",
        Handler::BsdProcessSockets);
    add(darwin::syscall::posix_semaphore_open, "posix_semaphore_open",
        Handler::BsdPosixSemaphore);
    add(darwin::syscall::posix_semaphore_close, "posix_semaphore_close",
        Handler::BsdPosixSemaphore);
    add(darwin::syscall::posix_semaphore_unlink, "posix_semaphore_unlink",
        Handler::BsdPosixSemaphore);
    add(darwin::syscall::posix_semaphore_wait, "posix_semaphore_wait",
        Handler::BsdPosixSemaphore);
    add(darwin::syscall::posix_semaphore_try_wait, "posix_semaphore_try_wait",
        Handler::BsdPosixSemaphore);
    add(darwin::syscall::posix_semaphore_post, "posix_semaphore_post",
        Handler::BsdPosixSemaphore);
    bind_bsd_contract_entries(table, abi);
    add(darwin::syscall::alternate_signal_stack, "alternate_signal_stack",
        Handler::BsdSignal);
    add(111, "sigsuspend", Handler::BsdSignal);
    add(darwin::syscall::pthread_kill, "pthread_kill", Handler::BsdSignal);
    add(darwin::syscall::kill, "kill", Handler::BsdSignal);
    add(darwin::syscall::get_host_uuid, "get_host_uuid", Handler::BsdPlatform);
    add(9, "link", Handler::BsdFilesystem);
    add(10, "unlink", Handler::BsdFilesystem);
    add(5, "open", Handler::BsdFilesystem);
    add(6, "close", Handler::BsdFilesystem);
    add(12, "chdir", Handler::BsdFilesystem);
    add(13, "fchdir", Handler::BsdFilesystem);
    add(darwin::syscall::change_mode, "change_mode", Handler::BsdFilesystem);
    add(darwin::syscall::change_owner, "change_owner", Handler::BsdFilesystem);
    add(darwin::syscall::change_owner_no_follow, "change_owner_no_follow",
        Handler::BsdFilesystem);
    add(18, "getfsstat", Handler::BsdFilesystem);
    add(33, "access", Handler::BsdFilesystem);
    add(darwin::syscall::change_flags, "change_flags", Handler::BsdFilesystem);
    add(darwin::syscall::change_flags_fd, "change_flags_fd",
        Handler::BsdFilesystem);
    add(darwin::syscall::change_owner_fd, "change_owner_fd",
        Handler::BsdFilesystem);
    add(darwin::syscall::change_mode_fd, "change_mode_fd",
        Handler::BsdFilesystem);
    add(darwin::syscall::change_mode_extended, "change_mode_extended",
        Handler::BsdFilesystem);
    add(darwin::syscall::change_mode_extended_fd, "change_mode_extended_fd",
        Handler::BsdFilesystem);
    add(darwin::syscall::flock, "flock", Handler::BsdFilesystem);
    add(darwin::syscall::synchronize_file, "synchronize_file",
        Handler::BsdFilesystem);
    add(36, "sync", Handler::BsdFilesystem);
    add(darwin::syscall::revoke, "revoke", Handler::BsdFilesystem);
    add(57, "symlink", Handler::BsdFilesystem);
    add(58, "readlink", Handler::BsdFilesystem);
    add(128, "rename", Handler::BsdFilesystem);
    add(136, "mkdir", Handler::BsdFilesystem);
    add(137, "rmdir", Handler::BsdFilesystem);
    add(darwin::syscall::update_file_times, "update_file_times",
        Handler::BsdFilesystem);
    add(darwin::syscall::update_file_times_fd, "update_file_times_fd",
        Handler::BsdFilesystem);
    add(153, "pread", Handler::BsdFilesystem);
    add(154, "pwrite", Handler::BsdFilesystem);
    add(157, "statfs", Handler::BsdFilesystem);
    add(159, "unmount", Handler::BsdFilesystem);
    add(167, "mount", Handler::BsdFilesystem);
    add(158, "fstatfs", Handler::BsdFilesystem);
    add(200, "truncate", Handler::BsdFilesystem);
    add(201, "ftruncate", Handler::BsdFilesystem);
    add(196, "getdirentries", Handler::BsdFilesystem);
    add(199, "lseek", Handler::BsdFilesystem);
    add(216, "mkcomplex", Handler::BsdFilesystem);
    add(220, "getattrlist", Handler::BsdFilesystem);
    add(221, "setattrlist", Handler::BsdFilesystem);
    add(344, "getdirentries64", Handler::BsdFilesystem);
    add(338, "stat64", Handler::BsdFilesystem);
    add(339, "fstat64", Handler::BsdFilesystem);
    add(340, "lstat64", Handler::BsdFilesystem);
    add(341, "stat64_extended", Handler::BsdFilesystem);
    add(342, "lstat64_extended", Handler::BsdFilesystem);
    add(343, "fstat64_extended", Handler::BsdFilesystem);
    add(345, "statfs64", Handler::BsdFilesystem);
    add(346, "fstatfs64", Handler::BsdFilesystem);
    add(347, "getfsstat64", Handler::BsdFilesystem);
    add(darwin::syscall::get_extended_attribute, "get_extended_attribute",
        Handler::BsdFilesystem);
    add(darwin::syscall::get_extended_attribute_fd, "get_extended_attribute_fd",
        Handler::BsdFilesystem);
    add(darwin::syscall::set_extended_attribute, "set_extended_attribute",
        Handler::BsdFilesystem);
    add(darwin::syscall::set_extended_attribute_fd, "set_extended_attribute_fd",
        Handler::BsdFilesystem);
    add(darwin::syscall::remove_extended_attribute, "remove_extended_attribute",
        Handler::BsdFilesystem);
    add(darwin::syscall::remove_extended_attribute_fd,
        "remove_extended_attribute_fd", Handler::BsdFilesystem);
    add(darwin::syscall::list_extended_attributes, "list_extended_attributes",
        Handler::BsdFilesystem);
    add(darwin::syscall::list_extended_attributes_fd,
        "list_extended_attributes_fd", Handler::BsdFilesystem);
    add(darwin::syscall::filesystem_control, "filesystem_control",
        Handler::BsdFilesystem);
    add(188, "stat", Handler::BsdFilesystem);
    add(190, "lstat", Handler::BsdFilesystem);
    add(189, "fstat", Handler::BsdFilesystem);
    add(darwin::syscall::read, "read", Handler::BsdDescriptorMemory);
    add(darwin::syscall::write, "write", Handler::BsdDescriptorMemory);
    add(41, "dup", Handler::BsdDescriptorMemory);
    add(42, "pipe", Handler::BsdDescriptorMemory);
    add(darwin::syscall::memory_synchronize, "memory_synchronize",
        Handler::BsdDescriptorMemory);
    add(73, "munmap", Handler::BsdDescriptorMemory);
    add(darwin::syscall::get_descriptor_table_size, "get_descriptor_table_size",
        Handler::BsdDescriptorMemory);
    add(darwin::syscall::duplicate_to, "duplicate_to",
        Handler::BsdDescriptorMemory);
    add(darwin::syscall::fcntl, "fcntl", Handler::BsdDescriptorMemory);
    add(darwin::syscall::file_descriptor_path_configuration,
        "file_descriptor_path_configuration", Handler::BsdDescriptorMemory);
    add(darwin::syscall::memory_protect, "memory_protect",
        Handler::BsdDescriptorMemory);
    add(darwin::syscall::memory_advise, "memory_advise",
        Handler::BsdDescriptorMemory);
    add(darwin::syscall::memory_lock, "memory_lock",
        Handler::BsdDescriptorMemory);
    add(darwin::syscall::memory_unlock, "memory_unlock",
        Handler::BsdDescriptorMemory);
    add(197, "mmap", Handler::BsdDescriptorMemory);
    add(266, "shm_open", Handler::BsdDescriptorMemory);
    add(267, "shm_unlink", Handler::BsdDescriptorMemory);
    add(294, "shared_region_check_np", Handler::BsdSharedRegion);
    add(295, "shared_region_map_np", Handler::BsdSharedRegion);
    add(297, "psynch_rw_longrdlock", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(298, "psynch_rw_yieldwrlock", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(299, "shared_region_map_file_np", Handler::BsdSharedRegion,
        Contract::LegacySharedRegion, true);
    add(300, "shared_region_make_private_np", Handler::BsdSharedRegion,
        Contract::LegacySharedRegion, true);
    add(301, "psynch_mutexwait", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(302, "psynch_mutexdrop", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(303, "psynch_cvbroad", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(304, "psynch_cvsignal", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(305, "psynch_cvwait", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(306, "psynch_rw_rdlock", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(307, "psynch_rw_wrlock", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(308, "psynch_rw_unlock", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(309, "psynch_rw_unlock2", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(312, "psynch_cvclrprepost", Handler::BsdPsynch, Contract::Psynch,
        abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1);
    add(438, "shared_region_map_and_slide_np", Handler::BsdSharedRegion,
        Contract::SharedRegionSlide,
        abi.shared_region_abi ==
            DarwinSharedRegionAbi::FixedMappingsWithSlideInfoV1);
    add(darwin::syscall::aio_synchronize, "aio_synchronize", Handler::BsdAio);
    add(darwin::syscall::aio_return, "aio_return", Handler::BsdAio);
    add(darwin::syscall::aio_suspend, "aio_suspend", Handler::BsdAio);
    add(darwin::syscall::aio_cancel, "aio_cancel", Handler::BsdAio);
    add(darwin::syscall::aio_error, "aio_error", Handler::BsdAio);
    add(darwin::syscall::aio_read, "aio_read", Handler::BsdAio);
    add(darwin::syscall::aio_write, "aio_write", Handler::BsdAio);
    add(darwin::syscall::ptrace, "ptrace", Handler::BsdDebug);
    add(180, "kdebug_trace", Handler::BsdDebug);
    add(27, "recvmsg", Handler::BsdSocket);
    add(28, "sendmsg", Handler::BsdSocket);
    add(darwin::syscall::receive_from, "receive_from", Handler::BsdSocket);
    add(darwin::syscall::accept, "accept", Handler::BsdSocket);
    add(31, "getpeername", Handler::BsdSocket);
    add(32, "getsockname", Handler::BsdSocket);
    add(darwin::syscall::socket, "socket", Handler::BsdSocket);
    add(darwin::syscall::connect, "connect", Handler::BsdSocket);
    add(darwin::syscall::bind, "bind", Handler::BsdSocket);
    add(105, "setsockopt", Handler::BsdSocket);
    add(darwin::syscall::listen, "listen", Handler::BsdSocket);
    add(118, "getsockopt", Handler::BsdSocket);
    add(darwin::syscall::write_vector, "write_vector", Handler::BsdSocket);
    add(darwin::syscall::send_to, "send_to", Handler::BsdSocket);
    add(darwin::syscall::shutdown, "shutdown", Handler::BsdSocket);
    add(darwin::syscall::socket_pair, "socket_pair", Handler::BsdSocket);
    add(54, "ioctl", Handler::BsdEvents);
    add(darwin::syscall::poll, "poll", Handler::BsdEvents);
    add(93, "select", Handler::BsdEvents);
    add(202, "__sysctl", Handler::BsdEvents);
    add(362, "kqueue", Handler::BsdKqueue);
    add(363, "kevent", Handler::BsdKqueue);
    add(369, "kevent64", Handler::BsdKqueue);
    add(darwin::syscall::code_signing_operations, "code_signing_operations",
        Handler::BsdCodeSigning);
    add(darwin::syscall::code_signing_audit_operations, "csops_audittoken",
        Handler::BsdCodeSigning);
    add(darwin::syscall::mac_syscall, "mac_syscall", Handler::BsdSecurity);
    add(darwin::syscall::get_audit_address, "get_audit_address",
        Handler::BsdAuditSession);
    add(darwin::syscall::audit_session_self, "audit_session_self",
        Handler::BsdAuditSession);
    add(darwin::syscall::audit_session_join, "audit_session_join",
        Handler::BsdAuditSession);
    add(darwin::syscall::audit_session_port, "audit_session_port",
        Handler::BsdAuditSession);
    add(darwin::syscall::fileport_makeport, "fileport_makeport",
        Handler::BsdFileport);
    add(darwin::syscall::fileport_makefd, "fileport_makefd",
        Handler::BsdFileport);
    if (abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1) {
        const auto replace = [&](std::uint32_t n, std::string_view previous,
                                 std::string_view next) {
            const Entry expected { Domain::BsdSyscall, n, n, previous,
                Handler::BsdSharedRegion, Contract::LegacySharedRegion,
                Cancellation::OriginalEntry, Outcome::HandlerValidated,
                "bsd/dispatch.cpp" };
            auto entry = expected;
            entry.operation = next;
            entry.handler = Handler::BsdPsynch;
            entry.contract = Contract::Psynch;
            table.replace_entry(expected, entry);
        };
        replace(299, "shared_region_map_file_np", "psynch_rw_downgrade");
        replace(300, "shared_region_make_private_np", "psynch_rw_upgrade");
    }
}
}
