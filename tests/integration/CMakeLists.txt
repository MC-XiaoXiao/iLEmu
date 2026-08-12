if(TARGET ilemu_guest_dns_probe)
    add_test(NAME inspect_guest_dns_probe
        COMMAND ilemu inspect
                --rootfs "${ILEMU_ROOTFS}"
                --binary /guest/dns_async_probe)
    set_tests_properties(inspect_guest_dns_probe PROPERTIES
        WORKING_DIRECTORY "${ILEMU_WORKSPACE_DIR}"
        PASS_REGULAR_EXPRESSION
            "cpu: ARMv6;file_type: executable;dyld: /usr/lib/dyld")
endif()

set(_launchd "${ILEMU_ROOTFS}/sbin/launchd")
if(EXISTS "${_launchd}")
    add_test(NAME inspect_real_launchd
        COMMAND ilemu inspect --rootfs "${ILEMU_ROOTFS}"
                                 --binary /sbin/launchd)
    add_test(NAME boot_real_launchd
        COMMAND ilemu boot --rootfs "${ILEMU_ROOTFS}"
                           --ticks 110000000
                           --activation preserve
                           --network isolated
                           --gles-backend software
                           --perf-summary)
    set_tests_properties(boot_real_launchd PROPERTIES
        WORKING_DIRECTORY "${ILEMU_WORKSPACE_DIR}"
        PASS_REGULAR_EXPRESSION
            "\\[boot\\] milestone=service-check-in service-state=ready"
        LABELS "slow;integration"
        TIMEOUT 90)
    set_tests_properties(inspect_real_launchd PROPERTIES
        WORKING_DIRECTORY "${ILEMU_WORKSPACE_DIR}")
endif()
