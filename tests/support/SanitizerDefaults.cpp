// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SanitizerDefaults.cpp - compile the sanitizer options INTO every test binary.
//
// Both sanitizers default to report-and-continue, which means a finding is
// printed and the process still exits 0 -- so ctest records a pass. That is not
// hypothetical: the first sanitized run of this suite reported 1824/1824 while
// UBSan was printing a real misaligned-access defect on every run, and it only
// surfaced once CI happened to set UBSAN_OPTIONS.
//
// Passing the options through the environment makes detection depend on how the
// suite is launched: build.sh --asan sets them, CI sets them, and `ctest` run by
// hand in a sanitized build directory does not. These hooks remove that
// dependency -- the sanitizer runtime calls them at startup, so the settings
// travel with the binary however it is invoked. An explicit ASAN_OPTIONS or
// UBSAN_OPTIONS in the environment still wins, so overriding stays possible.
//
// Compiled into every test target via asfw_test_common_interface; the bodies are
// inert in a non-sanitized build because nothing calls them.

extern "C" const char* __asan_default_options() {
    return "abort_on_error=1:halt_on_error=1:print_stacktrace=1";
}

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}
