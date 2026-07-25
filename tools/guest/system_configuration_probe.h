#pragma once

// Runs through the target firmware's public SystemConfiguration framework.
// Returns zero only after the dynamic-store and reachability callbacks have
// both executed on the guest CoreFoundation run loop.
int ilemu_run_system_configuration_probe(const char *reachability_name);
