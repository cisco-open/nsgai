# SPDX-License-Identifier: Apache-2.0
#
# Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
# All rights reserved.
# All rights reserved.

cmake_minimum_required(VERSION 3.16)
project(nsgai
        VERSION 0.1.0
        DESCRIPTION "Network-namespace-aware getaddrinfo replacement using c-ares"
        LANGUAGES C)

option(NSGAI_BUILD_TESTS "Build test programs" ON)
option(NSGAI_BUILD_PRELOAD "Build LD_PRELOAD shim" ON)
option(NSGAI_BUILD_NSS "Build NSS module" ON)
option(NSGAI_BUILD_EXAMPLES "Build example tools (nsgai_adig)" ON)
option(NSGAI_ENABLE_NETNS_TESTS
       "Add integration tests that require root + ip netns" OFF)
option(NSGAI_ENABLE_COVERAGE
       "Instrument library + tests with gcov/--coverage and add a 'coverage' target" OFF)

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
    find_package(cmocka CONFIG QUIET)
    if(cmocka_FOUND)
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
        "LINKER:--wrap=socket"
        "LINKER:--wrap=setns"
        "LINKER:--wrap=open"
        "LINKER:--wrap=fcntl"
        "LINKER:--wrap=ares_init_options"
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
        if(NSGAI_BUILD_EXAMPLES)
            add_test(NAME nsgai_adig_valgrind
                     COMMAND ${_vg} $<TARGET_FILE:nsgai_adig> -4 -c localhost)
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
            "NSGAI_LIB_DIR=$<TARGET_FILE_DIR:nsgai>;NSGAI_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};NSGAI_VALGRIND=1"
            FIXTURES_REQUIRED coredns
            SKIP_RETURN_CODE 77
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
if(NSGAI_BUILD_PRELOAD)
    install(TARGETS nsgai_preload LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(NSGAI_BUILD_NSS)
    install(TARGETS nss_nsgai LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# ----- coverage report targets -------------------------------------------
if(NSGAI_ENABLE_COVERAGE)
    find_program(LCOV_EXECUTABLE    lcov)
    find_program(GENHTML_EXECUTABLE genhtml)
    find_program(GCOVR_EXECUTABLE   gcovr)

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

    if(GCOVR_EXECUTABLE)
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_cov_dir}/html
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${GCOVR_EXECUTABLE}
                    --root ${CMAKE_CURRENT_SOURCE_DIR}
                    --filter ${CMAKE_CURRENT_SOURCE_DIR}/src
                    --filter ${CMAKE_CURRENT_SOURCE_DIR}/include
                    --gcov-ignore-parse-errors=negative_hits.warn_once_per_file
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
    elseif(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
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
    else()
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E echo
                "ERROR: NSGAI_ENABLE_COVERAGE=ON but neither lcov+genhtml "
                "nor gcovr were found. Install one of them and re-run "
                "cmake to generate a report."
            COMMAND ${CMAKE_COMMAND} -E false
            VERBATIM
        )
        message(WARNING
            "NSGAI_ENABLE_COVERAGE=ON: lcov/genhtml and gcovr not found; "
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

# ----- CPack: source and binary package generation -------------------------
# Generate source tarballs for release and optional binary packages (RPM/DEB).
# Usage:
#   cpack -G TGZ                    # source tarball
#   cpack -G RPM                    # binary RPM (Fedora/RHEL)
#   cpack -G DEB                    # binary DEB (Debian/Ubuntu)
#
# For Yocto/OpenEmbedded integration: 'cpack -G TGZ' produces source tarballs
# that bitbake recipes can fetch and package according to the recipe.

set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "Cisco Systems, Inc.")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Network-namespace-aware getaddrinfo replacement using c-ares")
set(CPACK_PACKAGE_DESCRIPTION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)

# Install prefix and layout.
set(CPACK_PACKAGING_INSTALL_PREFIX /)
set(CPACK_CMAKE_GENERATOR "Unix Makefiles")

# Generator selection: default TGZ for portability.
set(CPACK_GENERATOR TGZ)
if(CMAKE_SYSTEM_NAME MATCHES Linux)
    # On Linux distros, offer RPM and DEB as alternatives.
    list(APPEND CPACK_GENERATOR RPM DEB)
endif()

# RPM settings.
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_URL "https://github.com/cisco-open/nsgai")
set(CPACK_RPM_PACKAGE_SUMMARY ${CPACK_PACKAGE_DESCRIPTION_SUMMARY})

# DEB settings.
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Cisco Systems, Inc. <devx@cisco.com>")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/cisco-open/nsgai")

include(CPack)
