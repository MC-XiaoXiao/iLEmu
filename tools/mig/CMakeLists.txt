# Darwin's original MIG executable is tied to the Darwin toolchain. This
# host-side generator consumes authoritative .defs files and emits the ARM32
# wire metadata used by the compatibility adapter.
add_executable(ilemu_mig_id_gen mig_id_gen.cpp)
target_compile_features(ilemu_mig_id_gen PRIVATE cxx_std_20)
target_compile_options(ilemu_mig_id_gen PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)

set(ILEMU_XNU792_DIR
    "${ILEMU_SOURCES_DIR}/xnu-792.24.17")
set(ILEMU_CONFIGD137_DIR
    "${ILEMU_SOURCES_DIR}/configd-137.3")
set(ILEMU_LAUNCHD257_DIR
    "${ILEMU_SOURCES_DIR}/launchd-257/launchd/src")
set(ILEMU_MIG_GENERATED_DIR
    "${PROJECT_BINARY_DIR}/generated/ilemu")
set(ILEMU_MIG_GENERATED_HEADERS)

function(ilemu_generate_mig_ids name definition)
    set(output "${ILEMU_MIG_GENERATED_DIR}/${name}_mig_ids.hpp")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND $<TARGET_FILE:ilemu_mig_id_gen>
                --input "${definition}"
                --output "${output}"
                --namespace "${name}"
                --include-dir "${ILEMU_XNU792_DIR}/osfmk"
                --include-dir "${ILEMU_LAUNCHD257_DIR}"
                --type-def "${ILEMU_SOURCES_DIR}/xnu/osfmk/mach/machine/machine_types.defs"
                # launchd-257's private header typedefs this as char*, while
                # job_mig_spawn receives the generated vm_offset_t/count pair.
                --type-alias "_internal_string_t=pointer_t"
        DEPENDS ilemu_mig_id_gen "${definition}"
                "${ILEMU_XNU792_DIR}/osfmk/mach/std_types.defs"
                "${ILEMU_XNU792_DIR}/osfmk/mach/mach_types.defs"
                "${ILEMU_SOURCES_DIR}/xnu/osfmk/mach/machine/machine_types.defs"
                "${ILEMU_LAUNCHD257_DIR}/launchd_mig_types.defs"
        VERBATIM
        COMMENT "Generating ${name} MIG metadata from source definitions")
    list(APPEND ILEMU_MIG_GENERATED_HEADERS "${output}")
    set(ILEMU_MIG_GENERATED_HEADERS
        "${ILEMU_MIG_GENERATED_HEADERS}" PARENT_SCOPE)
endfunction()

ilemu_generate_mig_ids(
    mach_port "${ILEMU_XNU792_DIR}/osfmk/mach/mach_port.defs")
ilemu_generate_mig_ids(
    task "${ILEMU_XNU792_DIR}/osfmk/mach/task.defs")
ilemu_generate_mig_ids(
    thread_act "${ILEMU_XNU792_DIR}/osfmk/mach/thread_act.defs")
ilemu_generate_mig_ids(
    device "${ILEMU_XNU792_DIR}/osfmk/device/device.defs")
ilemu_generate_mig_ids(
    mach_host "${ILEMU_XNU792_DIR}/osfmk/mach/mach_host.defs")
ilemu_generate_mig_ids(
    host_priv "${ILEMU_XNU792_DIR}/osfmk/mach/host_priv.defs")
ilemu_generate_mig_ids(
    processor_set "${ILEMU_XNU792_DIR}/osfmk/mach/processor_set.defs")
ilemu_generate_mig_ids(
    vm_map "${ILEMU_XNU792_DIR}/osfmk/mach/vm_map.defs")
ilemu_generate_mig_ids(
    clock "${ILEMU_XNU792_DIR}/osfmk/mach/clock.defs")
ilemu_generate_mig_ids(
    clock_reply "${ILEMU_XNU792_DIR}/osfmk/mach/clock_reply.defs")
ilemu_generate_mig_ids(
    semaphore "${ILEMU_XNU792_DIR}/osfmk/mach/semaphore.defs")
ilemu_generate_mig_ids(
    bootstrap "${ILEMU_LAUNCHD257_DIR}/protocol_jobmgr.defs")
ilemu_generate_mig_ids(
    protocol_vproc "${ILEMU_LAUNCHD257_DIR}/protocol_job.defs")
ilemu_generate_mig_ids(
    system_configuration
    "${ILEMU_CONFIGD137_DIR}/SystemConfiguration.fproj/config.defs")

add_custom_target(ilemu_mig_metadata
    DEPENDS ${ILEMU_MIG_GENERATED_HEADERS})

# Publish generated headers to sibling subsystem directories configured later.
set(ILEMU_MIG_GENERATED_HEADERS
    "${ILEMU_MIG_GENERATED_HEADERS}" PARENT_SCOPE)
