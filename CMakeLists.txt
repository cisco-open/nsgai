# SPDX-License-Identifier: Apache-2.0
#
# Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
# All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.16)
project(nsgai
        VERSION 0.2.1
        DESCRIPTION "Network-namespace-aware getaddrinfo replacement using c-ares"
        LANGUAGES C)

set(NSGAI_VERSION_SUFFIX "-dev")
set(NSGAI_PROJECT_VERSION_DISPLAY "${PROJECT_VERSION}${NSGAI_VERSION_SUFFIX}")

# ----- source SBOM -------------------------------------------------------
# The source tree contains the template; CMake writes release-specific
# metadata to the build tree so it can be attached to a release separately.
set(NSGAI_SPDX_NAMESPACE_BASE
    "https://github.com/cisco-open/nsgai/spdx"
    CACHE STRING "Base URL for generated SPDX document namespaces")
set(NSGAI_SPDX_SOURCE_DOWNLOAD_LOCATION "https://github.com/cisco-open/nsgai" CACHE STRING
    "Source archive URL recorded in the generated SPDX document")
if(NOT DEFINED NSGAI_SPDX_CREATED)
    string(TIMESTAMP _nsgai_spdx_created "%Y-%m-%dT%H:%M:%SZ" UTC)
    set(NSGAI_SPDX_CREATED "${_nsgai_spdx_created}" CACHE STRING
        "SPDX creation time in UTC; set explicitly for reproducible release builds")
endif()

set(NSGAI_SPDX_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/sbom")
set(NSGAI_SPDX_OUTPUT
    "${NSGAI_SPDX_OUTPUT_DIR}/nsgai-${PROJECT_VERSION}-source.spdx.json")
file(MAKE_DIRECTORY "${NSGAI_SPDX_OUTPUT_DIR}")
configure_file(sbom/nsgai-source.spdx.json.in "${NSGAI_SPDX_OUTPUT}" @ONLY)
add_custom_target(source-sbom DEPENDS "${NSGAI_SPDX_OUTPUT}")

option(NSGAI_BUILD_TESTS "Build test programs" ON)
option(NSGAI_BUILD_PRELOAD "Build LD_PRELOAD shim" ON)
option(NSGAI_BUILD_NSS "Build NSS module" ON)
option(NSGAI_BUILD_EXAMPLES "Build example tools (nsgai_adig)" ON)
option(NSGAI_ENABLE_NETNS_TESTS
       "Register integration tests that require root + ip netns" ON)
option(NSGAI_ENABLE_COVERAGE
       "Instrument library + tests with gcov/--coverage and add a 'coverage' target" OFF)
set(NSGAI_COVERAGE_TOOL "auto" CACHE STRING
    "Coverage report backend: auto, lcov, or gcovr")
set_property(CACHE NSGAI_COVERAGE_TOOL PROPERTY STRINGS auto lcov gcovr)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Feature-test macro must be provided by compiler flags, not per-file,
# so all headers (including forced/includes-from-command-line) see it.
add_compile_definitions(_GNU_SOURCE)

# Default to "Release with debug info" if user did not specify.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
add_compile_options(
    -Wall
    -Wextra
    -Wformat
    -Wformat-security
    -Werror=format-security
    -Werror
    -Wswitch-enum       # every enum value must appear in switch; no default
    -Wno-unused-parameter
)

# ----- coverage instrumentation ------------------------------------------
# When enabled, every C target below is compiled and linked with --coverage
# (gcov). A `coverage` custom target collects .gcda data after ctest and
# produces an HTML report under build/coverage/ via lcov+genhtml (preferred)
# or gcovr as a fallback.
if(NSGAI_ENABLE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(WARNING
            "NSGAI_ENABLE_COVERAGE: untested with compiler "
            "'${CMAKE_C_COMPILER_ID}'; --coverage may not apply.")
    endif()
    if(NOT NSGAI_COVERAGE_TOOL MATCHES "^(auto|lcov|gcovr)$")
        message(FATAL_ERROR
            "NSGAI_COVERAGE_TOOL must be one of: auto, lcov, gcovr")
    endif()
    # Force an un-optimized build so line counts in the report are meaningful.
    # Users can override by setting -DCMAKE_BUILD_TYPE=... explicitly *and*
    # accepting that inlining/folding will skew coverage.
    if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        message(STATUS "NSGAI_ENABLE_COVERAGE: forcing CMAKE_BUILD_TYPE=Debug")
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
    endif()
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(ClangFormat)

# ----- c-ares dependency -------------------------------------------------
# Prefer a config package; fall back to pkg-config.
find_package(c-ares CONFIG QUIET)
if(NOT c-ares_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CARES REQUIRED IMPORTED_TARGET libcares)
    add_library(cares_interface INTERFACE)
    target_link_libraries(cares_interface INTERFACE PkgConfig::CARES)
    set(CARES_TARGET cares_interface)
else()
    set(CARES_TARGET c-ares::cares)
endif()

# ----- cmocka (required when tests are enabled) ---------------------------
# Used by white-box tests and preload tests for per-test pass/fail reports.
if(NSGAI_BUILD_TESTS)
    # Some distros (e.g. Ubuntu 22.04's libcmocka-dev) ship a CMake config
    # package that reports cmocka_FOUND=TRUE without actually defining the
    # cmocka::cmocka import target, so check for the target too before
    # trusting the CONFIG result.
    find_package(cmocka CONFIG QUIET)
    if(cmocka_FOUND AND TARGET cmocka::cmocka)
        set(NSGAI_CMOCKA_TARGET cmocka::cmocka)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(CMOCKA REQUIRED IMPORTED_TARGET cmocka)
        set(NSGAI_CMOCKA_TARGET PkgConfig::CMOCKA)
    endif()
    message(STATUS "cmocka found - unit tests enabled")
endif()

# ----- libnsgai ----------------------------------------------------------
# Core sources are compiled once into an OBJECT library so the NSS module
# can reuse them directly without relying on (now-hidden) internal symbols
# being exported from libnsgai.so.
add_library(nsgai_core OBJECT
    src/nsgai_diag.c
    src/nsgai_ns.c
    src/nsgai_channel.c
    src/nsgai_addrinfo.c
    src/nsgai_async.c
    src/nsgai_cleanup.c
)
target_include_directories(nsgai_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(nsgai_core PUBLIC ${CARES_TARGET})

add_library(nsgai SHARED $<TARGET_OBJECTS:nsgai_core>)
target_include_directories(nsgai
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(nsgai PUBLIC ${CARES_TARGET})
target_link_libraries(nsgai PRIVATE pthread)
set_target_properties(nsgai PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET default     # public API is exported
)

# Hide internal symbols via an ELF version script.  Only the documented
# nsgai_* entry points and the weak POSIX-named aliases are visible.
# Linker support for --version-script is universal on GNU/LLD; we feed
# it unconditionally and let configure-time fail loudly if absent.
set(_nsgai_version_script ${CMAKE_CURRENT_SOURCE_DIR}/src/nsgai.map)
target_link_options(nsgai PRIVATE
    "LINKER:--version-script=${_nsgai_version_script}")
set_target_properties(nsgai PROPERTIES
    LINK_DEPENDS ${_nsgai_version_script})

# ----- libnsgai_preload.so ----------------------------------------------
if(NSGAI_BUILD_PRELOAD)
    add_library(nsgai_preload SHARED src/preload.c)
    target_include_directories(nsgai_preload PRIVATE include src)
    target_link_libraries(nsgai_preload PRIVATE nsgai dl pthread)
    set_target_properties(nsgai_preload PROPERTIES
        OUTPUT_NAME   nsgai_preload
        VERSION       ${PROJECT_VERSION}
        SOVERSION     ${PROJECT_VERSION_MAJOR}
        C_VISIBILITY_PRESET default
    )
endif()

# ----- libnss_nsgai.so.2 -------------------------------------------------
if(NSGAI_BUILD_NSS)
    # NSS module embeds nsgai_core directly rather than linking libnsgai.so,
    # since libnsgai.so now hides internal helpers (channel/ns/etc.) behind
    # its version script.  We compile nss_nsgai.c as a separate OBJECT
    # library so the same .o is shared by libnss_nsgai.so.2 and the
    # failure-injection unit test, allowing coverage counters to merge
    # across both.
    add_library(nss_nsgai_core OBJECT src/nss_nsgai.c)
    target_include_directories(nss_nsgai_core PRIVATE include src)
    target_link_libraries(nss_nsgai_core PRIVATE ${CARES_TARGET})

    add_library(nss_nsgai SHARED
        $<TARGET_OBJECTS:nss_nsgai_core>
        $<TARGET_OBJECTS:nsgai_core>
    )
    target_include_directories(nss_nsgai PRIVATE include src)
    target_link_libraries(nss_nsgai PRIVATE ${CARES_TARGET} pthread)
    set_target_properties(nss_nsgai PROPERTIES
        OUTPUT_NAME   nss_nsgai
        SOVERSION     2
        VERSION       2          # produces libnss_nsgai.so.2
        C_VISIBILITY_PRESET default
        PREFIX        "lib"
    )
endif()

# ----- tests --------------------------------------------------------------
if(NSGAI_BUILD_TESTS)
    add_executable(nsgai_test test/test_basic.c)
    target_link_libraries(nsgai_test PRIVATE nsgai)

    add_executable(test_multins test/test_multins.c)
    target_link_libraries(test_multins PRIVATE nsgai pthread)

    # Coverage-focused unit test for the public API.
    add_executable(test_api_coverage test/test_api_coverage.c)
    target_link_libraries(test_api_coverage PRIVATE nsgai)

    # Dedicated regression for rsyslog-style node==NULL getaddrinfo usage.
    add_executable(test_rsyslog_null_node_regression
        test/test_rsyslog_null_node_regression.c)
    target_link_libraries(test_rsyslog_null_node_regression PRIVATE nsgai)

    add_executable(test_teardown_dlopen test/test_teardown_dlopen.c)
    target_include_directories(test_teardown_dlopen PRIVATE include)
    target_link_libraries(test_teardown_dlopen PRIVATE ${CMAKE_DL_LIBS})

    add_executable(test_async_api
        test/test_async_api.c
        $<TARGET_OBJECTS:nsgai_core>)
    target_include_directories(test_async_api PRIVATE include src)
    target_link_libraries(test_async_api PRIVATE ${CARES_TARGET} pthread)
    target_link_options(test_async_api PRIVATE
        "LINKER:--wrap=calloc"
        "LINKER:--wrap=pthread_create"
        "LINKER:--wrap=clock_gettime"
        "LINKER:--wrap=pthread_cond_timedwait"
    )

    # Coverage-focused test for the per-namespace channel cache.
    # Requires CAP_SYS_ADMIN for unshare(CLONE_NEWNET); self-skips
    # (exit 77) when the cap is unavailable.
    add_executable(test_channel_cache test/test_channel_cache.c)
    target_link_libraries(test_channel_cache PRIVATE nsgai)

    # Driven by integration_dns_netns.sh.  Exercises the cache stale-
    # revalidation path by mtime-bumping a per-ns resolv.conf between
    # two lookups in the same process.
    add_executable(test_cache_stale test/test_cache_stale.c)
    target_link_libraries(test_cache_stale PRIVATE nsgai)

    # Failure-injection test.  Links the same OBJECT files used by
    # libnsgai.so so coverage counters merge with the main report,
    # and uses ld --wrap to interpose libc/c-ares symbols, allowing
    # us to drive otherwise unreachable error branches inside
    # nsgai_channel_create / nsgai_ns_resolve_*.
    add_executable(test_channel_failures
        test/test_channel_failures.c
        $<TARGET_OBJECTS:nsgai_core>)
    target_include_directories(test_channel_failures PRIVATE include src)
    target_link_libraries(test_channel_failures PRIVATE ${CARES_TARGET} pthread)
    target_link_options(test_channel_failures PRIVATE
        "LINKER:--wrap=ares_init_options"
        "LINKER:--wrap=ares_getaddrinfo"
        "LINKER:--wrap=ares_destroy"
        "LINKER:--wrap=fcntl"
    )

    # NSS failure-injection coverage test.  Links the SAME OBJECT files
    # used by libnss_nsgai.so.2 + libnsgai.so so coverage counters from
    # the mocked failure paths land in the project-wide report.
    if(NSGAI_BUILD_NSS)
        add_executable(test_nss_failures
            test/test_nss_failures.c
            $<TARGET_OBJECTS:nss_nsgai_core>
            $<TARGET_OBJECTS:nsgai_core>)
        target_include_directories(test_nss_failures PRIVATE include src)
        target_link_libraries(test_nss_failures
            PRIVATE ${CARES_TARGET} pthread)
        target_link_options(test_nss_failures PRIVATE
            "LINKER:--wrap=ares_init_options"
            "LINKER:--wrap=ares_getaddrinfo"
            "LINKER:--wrap=nsgai_getnameinfo"
        )
    endif()

    # cmocka-based white-box tests with --wrap interposition.  Re-
    # compile the SUT sources directly (no link against libnsgai) so
    # we can drive file-local statics and inject libc-level failures.
    add_executable(test_channel_mocks
        test/test_channel_mocks.c
        src/nsgai_diag.c)
    target_include_directories(test_channel_mocks
        PRIVATE include src)
    target_link_libraries(test_channel_mocks
        PRIVATE ${NSGAI_CMOCKA_TARGET} ${CARES_TARGET} pthread)
    # Tell ld to redirect each listed libc symbol to __wrap_<sym>
    # within this executable only.  Tests provide both __wrap_<sym>
    # (which can opt to call __real_<sym>) and trust the rest of the
    # process (cmocka, libc helpers) to be unaffected because each
    # wrapper defaults to passthrough.
    target_link_options(test_channel_mocks PRIVATE
        "LINKER:--wrap=calloc"
        "LINKER:--wrap=socket"
        "LINKER:--wrap=setns"
        "LINKER:--wrap=open"
        "LINKER:--wrap=open64"
        "LINKER:--wrap=__open_2"
        "LINKER:--wrap=fcntl"
        "LINKER:--wrap=ares_init_options"
        "LINKER:--wrap=ares_getaddrinfo"
        "LINKER:--wrap=ares_set_socket_functions_ex"
        "LINKER:--wrap=pthread_mutex_lock"
        "LINKER:--wrap=pthread_mutex_unlock"
        "LINKER:--wrap=pthread_cond_wait"
        "LINKER:--wrap=pthread_cond_signal"
        "LINKER:--wrap=pthread_mutex_init"
        "LINKER:--wrap=pthread_cond_init"
        "LINKER:--wrap=pthread_mutex_destroy"
        "LINKER:--wrap=pthread_cond_destroy"
        "LINKER:--wrap=abort"
    )

    # Coverage-focused unit test for the NSS module.  Loads
    # libnss_nsgai.so.2 by absolute path so we don't need to install it
    # into /lib*/libnss_*; only links against libdl.
    if(NSGAI_BUILD_NSS)
        add_executable(test_nss test/test_nss.c)
        target_link_libraries(test_nss PRIVATE dl)
    endif()

    # Coverage-focused unit test for the LD_PRELOAD shim.  Deliberately
    # does not link libnsgai; the preload library is injected at run time
    # via LD_PRELOAD in the corresponding ctest entry.
    if(NSGAI_BUILD_PRELOAD)
        add_executable(test_preload test/test_preload.c)
        target_link_libraries(test_preload PRIVATE ${NSGAI_CMOCKA_TARGET})
    endif()

    enable_testing()
    add_test(NAME nsgai_basic COMMAND nsgai_test)
    add_test(NAME nsgai_api_coverage COMMAND test_api_coverage)
    add_test(NAME nsgai_rsyslog_null_node_regression
             COMMAND test_rsyslog_null_node_regression)
    add_test(NAME nsgai_teardown_dlopen
             COMMAND test_teardown_dlopen $<TARGET_FILE:nsgai>)
    add_test(NAME nsgai_async_api COMMAND test_async_api)
    set_tests_properties(nsgai_async_api PROPERTIES TIMEOUT 30)
    add_test(NAME nsgai_api_coverage_preload COMMAND test_api_coverage)
    set_tests_properties(nsgai_api_coverage_preload PROPERTIES
        ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:nsgai>")
    add_test(NAME nsgai_channel_failures COMMAND test_channel_failures)
    if(NSGAI_BUILD_NSS)
        add_test(NAME nsgai_nss_failures COMMAND test_nss_failures)
    endif()
    add_test(NAME nsgai_channel_cache COMMAND test_channel_cache)
    set_tests_properties(nsgai_channel_cache PROPERTIES
        SKIP_RETURN_CODE 77)

    add_test(NAME nsgai_channel_mocks COMMAND test_channel_mocks)

    if(NSGAI_BUILD_NSS)
        add_test(NAME nsgai_nss_unit
                 COMMAND test_nss $<TARGET_FILE:nss_nsgai>)
    endif()

    if(NSGAI_BUILD_PRELOAD)
        add_test(NAME nsgai_preload_unit COMMAND test_preload)
        set_tests_properties(nsgai_preload_unit PROPERTIES
            ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:nsgai_preload>")
    endif()

    # Optional valgrind wrappers for the in-tree C tests.
    find_program(VALGRIND_EXECUTABLE valgrind)
    if(VALGRIND_EXECUTABLE)
        set(_vg ${VALGRIND_EXECUTABLE}
                --error-exitcode=1
                --leak-check=full
                --errors-for-leak-kinds=definite
                --show-leak-kinds=definite
                --track-fds=no
                --quiet)
        add_test(NAME nsgai_basic_valgrind
                 COMMAND ${_vg} $<TARGET_FILE:nsgai_test>)
        add_test(NAME nsgai_api_coverage_valgrind
                 COMMAND ${_vg} $<TARGET_FILE:test_api_coverage>)
        add_test(NAME nsgai_rsyslog_null_node_regression_valgrind
                 COMMAND ${_vg} $<TARGET_FILE:test_rsyslog_null_node_regression>)
        add_test(NAME nsgai_teardown_dlopen_valgrind
                 COMMAND ${_vg} $<TARGET_FILE:test_teardown_dlopen>
                         $<TARGET_FILE:nsgai>)
        add_test(NAME nsgai_async_api_valgrind
                 COMMAND ${_vg} $<TARGET_FILE:test_async_api>)
        if(NSGAI_BUILD_EXAMPLES)
            add_test(NAME nsgai_adig_valgrind
                     COMMAND ${_vg} $<TARGET_FILE:nsgai_adig> -4 -c localhost)
        endif()

        # Safety net: some valgrind/glibc combinations (observed with
        # valgrind 3.22 on Ubuntu 24.04's glibc 2.39) can deadlock in
        # signal/futex codepaths rather than fail cleanly. A generous
        # TIMEOUT turns a CI-hanging hang into a normal test failure.
        set_tests_properties(
            nsgai_basic_valgrind
            nsgai_api_coverage_valgrind
            nsgai_rsyslog_null_node_regression_valgrind
            nsgai_teardown_dlopen_valgrind
            nsgai_async_api_valgrind
            PROPERTIES TIMEOUT 60)
        if(NSGAI_BUILD_EXAMPLES)
            set_tests_properties(nsgai_adig_valgrind PROPERTIES TIMEOUT 60)
        endif()
    endif()
endif()

# ----- examples -----------------------------------------------------------
if(NSGAI_BUILD_EXAMPLES)
    add_executable(nsgai_adig examples/nsgai_adig.c)
    target_link_libraries(nsgai_adig PRIVATE nsgai)
    install(TARGETS nsgai_adig RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ----- integration tests (root + ip netns) --------------------------------
if(NSGAI_BUILD_TESTS AND NSGAI_ENABLE_NETNS_TESTS AND NSGAI_BUILD_EXAMPLES)
    add_test(
        NAME nsgai_netns_integration
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/integration_netns.sh
                $<TARGET_FILE:nsgai_adig>
    )
    set_tests_properties(nsgai_netns_integration PROPERTIES
        ENVIRONMENT "NSGAI_LIB_DIR=$<TARGET_FILE_DIR:nsgai>"
        SKIP_RETURN_CODE 77
    )

    # Fixture: fetch CoreDNS once before any DNS-based netns test runs.
    # The script is idempotent (no-op if build/coredns already exists).
    add_test(
        NAME nsgai_fetch_coredns
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/fetch_coredns.sh
                ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(nsgai_fetch_coredns PROPERTIES
        FIXTURES_SETUP coredns
        SKIP_RETURN_CODE 77
    )

    add_test(
        NAME nsgai_dns_netns_integration
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/integration_dns_netns.sh
                $<TARGET_FILE:nsgai_adig>
                $<TARGET_FILE:test_multins>
                $<TARGET_FILE:test_cache_stale>
    )
    set_tests_properties(nsgai_dns_netns_integration PROPERTIES
        ENVIRONMENT
        "NSGAI_LIB_DIR=$<TARGET_FILE_DIR:nsgai>;NSGAI_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        FIXTURES_REQUIRED coredns
        SKIP_RETURN_CODE 77
    )

    add_test(
        NAME nsgai_policy_netns_integration
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/integration_policy_netns.sh
                $<TARGET_FILE:nsgai_adig>
    )
    set_tests_properties(nsgai_policy_netns_integration PROPERTIES
        ENVIRONMENT
        "NSGAI_LIB_DIR=$<TARGET_FILE_DIR:nsgai>;NSGAI_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        FIXTURES_REQUIRED coredns
        SKIP_RETURN_CODE 77
    )

    if(VALGRIND_EXECUTABLE)
        add_test(
            NAME nsgai_dns_netns_integration_valgrind
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/integration_dns_netns.sh
                    $<TARGET_FILE:nsgai_adig>
                    $<TARGET_FILE:test_multins>
                    $<TARGET_FILE:test_cache_stale>
        )
        set_tests_properties(nsgai_dns_netns_integration_valgrind PROPERTIES
            ENVIRONMENT
            "NSGAI_LIB_DIR=$<TARGET_FILE_DIR:nsgai>;NSGAI_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};NSGAI_VALGRIND=1;NSGAI_VALGRIND_BIN=${VALGRIND_EXECUTABLE}"
            FIXTURES_REQUIRED coredns
            SKIP_RETURN_CODE 77
            TIMEOUT 120
        )
    endif()

    # Convenience build target for users who want to pre-fetch manually:
    #   cmake --build build --target fetch-coredns
    add_custom_target(fetch-coredns
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/fetch_coredns.sh
                ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Fetching CoreDNS for DNS netns integration tests"
        VERBATIM
    )
endif()

# ----- install ------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS nsgai
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/nsgai.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
configure_file(nsgai.pc.in nsgai.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nsgai.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
if(NSGAI_BUILD_PRELOAD)
    install(TARGETS nsgai_preload LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(NSGAI_BUILD_NSS)
    install(TARGETS nss_nsgai LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# ----- CPack: downstream package generation -------------------------------
# CPack is available for local use and downstream distribution packaging.
# Cisco source releases do not attach the generated binary packages.
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "Cisco Systems, Inc.")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Network-namespace-aware getaddrinfo replacement using c-ares")
set(CPACK_PACKAGE_DESCRIPTION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGING_INSTALL_PREFIX /)
set(CPACK_CMAKE_GENERATOR "Unix Makefiles")
set(CPACK_GENERATOR TGZ)
if(CMAKE_SYSTEM_NAME MATCHES Linux)
    list(APPEND CPACK_GENERATOR RPM DEB)
endif()
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_URL "https://github.com/cisco-open/nsgai")
set(CPACK_RPM_PACKAGE_SUMMARY ${CPACK_PACKAGE_DESCRIPTION_SUMMARY})
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Cisco Systems, Inc. <devx@cisco.com>")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/cisco-open/nsgai")
include(CPack)
# ----- coverage report targets -------------------------------------------
if(NSGAI_ENABLE_COVERAGE)
    find_program(LCOV_EXECUTABLE    lcov)
    find_program(GENHTML_EXECUTABLE genhtml)
    find_program(GCOVR_EXECUTABLE   gcovr)

    # gcovr < 6.0 (e.g. Ubuntu 22.04's packaged 5.0) only accepts
    # --gcov-ignore-parse-errors as a bare boolean flag; the fine-grained
    # "=negative_hits.warn_once_per_file" argument form is 6.0+ only.
    set(_gcovr_ignore_parse_errors_flag --gcov-ignore-parse-errors)
    if(GCOVR_EXECUTABLE)
        execute_process(
            COMMAND ${GCOVR_EXECUTABLE} --version
            OUTPUT_VARIABLE _gcovr_version_output
            ERROR_QUIET)
        string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" _gcovr_version "${_gcovr_version_output}")
        if(_gcovr_version AND _gcovr_version VERSION_GREATER_EQUAL "6.0")
            set(_gcovr_ignore_parse_errors_flag
                --gcov-ignore-parse-errors=negative_hits.warn_once_per_file)
        endif()
    endif()

    set(_cov_dir  ${CMAKE_CURRENT_BINARY_DIR}/coverage)
    set(_cov_info ${_cov_dir}/coverage.info)

    # Wipe any stale .gcda counters before re-running tests, so a fresh
    # `cmake --build . --target coverage` reflects only the latest run.
    add_custom_target(coverage-clean
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_cov_dir}
        COMMAND find ${CMAKE_CURRENT_BINARY_DIR} -name '*.gcda' -delete
        COMMENT "Removing stale .gcda counters and previous coverage report"
        VERBATIM
    )

    set(_use_lcov FALSE)
    set(_use_gcovr FALSE)
    if(NSGAI_COVERAGE_TOOL STREQUAL "auto")
        if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
            set(_use_lcov TRUE)
        elseif(GCOVR_EXECUTABLE)
            set(_use_gcovr TRUE)
        endif()
    elseif(NSGAI_COVERAGE_TOOL STREQUAL "lcov")
        if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
            set(_use_lcov TRUE)
        else()
            message(FATAL_ERROR
                "NSGAI_COVERAGE_TOOL=lcov requires both lcov and genhtml")
        endif()
    elseif(NSGAI_COVERAGE_TOOL STREQUAL "gcovr")
        if(GCOVR_EXECUTABLE)
            set(_use_gcovr TRUE)
        else()
            message(FATAL_ERROR
                "NSGAI_COVERAGE_TOOL=gcovr requires gcovr")
        endif()
    endif()

    if(_use_lcov)
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_cov_dir}
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${LCOV_EXECUTABLE}
                    --quiet
                    --capture
                    --directory ${CMAKE_CURRENT_BINARY_DIR}
                    --base-directory ${CMAKE_CURRENT_SOURCE_DIR}
                    --rc lcov_branch_coverage=1
                    --ignore-errors mismatch,negative,source,unused
                    --output-file ${_cov_info}.raw
            COMMAND ${LCOV_EXECUTABLE}
                    --quiet
                    --extract ${_cov_info}.raw
                        '${CMAKE_CURRENT_SOURCE_DIR}/src/*'
                        '${CMAKE_CURRENT_SOURCE_DIR}/include/*'
                    --rc lcov_branch_coverage=1
                    --output-file ${_cov_info}
            COMMAND ${GENHTML_EXECUTABLE}
                    --quiet
                    --branch-coverage
                    --legend
                    --demangle-cpp
                    --output-directory ${_cov_dir}/html
                    ${_cov_info}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "Coverage report: ${_cov_dir}/html/index.html"
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Running ctest and generating lcov HTML coverage report"
            VERBATIM
            USES_TERMINAL
        )
    elseif(_use_gcovr)
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_cov_dir}/html
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${GCOVR_EXECUTABLE}
                    --root ${CMAKE_CURRENT_SOURCE_DIR}
                    --filter ${CMAKE_CURRENT_SOURCE_DIR}/src
                    --filter ${CMAKE_CURRENT_SOURCE_DIR}/include
                    ${_gcovr_ignore_parse_errors_flag}
                    --html-details ${_cov_dir}/html/index.html
                    --print-summary
                    ${CMAKE_CURRENT_BINARY_DIR}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "Coverage report: ${_cov_dir}/html/index.html"
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Running ctest and generating gcovr HTML coverage report"
            VERBATIM
            USES_TERMINAL
        )
    else()
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E echo
                "ERROR: NSGAI_ENABLE_COVERAGE=ON but no usable coverage "
                "backend was found for NSGAI_COVERAGE_TOOL=${NSGAI_COVERAGE_TOOL}."
            COMMAND ${CMAKE_COMMAND} -E false
            VERBATIM
        )
        message(WARNING
            "NSGAI_ENABLE_COVERAGE=ON: no usable coverage backend was found; "
            "'coverage' target will be a stub. .gcno/.gcda files will still "
            "be produced and can be inspected with gcov(1) manually.")
    endif()
endif()

# ----- Doxygen documentation target ('docs') -----------------------------
find_package(Doxygen OPTIONAL_COMPONENTS dot)

if(DOXYGEN_FOUND)
    set(_doxy_in  ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in)
    set(_doxy_out ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    set(_doxy_dir ${CMAKE_CURRENT_BINARY_DIR}/docs/html)

    if(DOXYGEN_DOT_FOUND)
        set(_DOXY_HAVE_DOT YES)
    else()
        set(_DOXY_HAVE_DOT NO)
    endif()

    configure_file(${_doxy_in} ${_doxy_out} @ONLY)

    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${_doxy_out}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating Doxygen HTML → ${_doxy_dir}/index.html"
        VERBATIM
        USES_TERMINAL
    )
else()
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E echo
            "ERROR: doxygen not found; install it and re-run cmake."
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
    message(STATUS "Doxygen not found; 'docs' target will be a stub.")
endif()
