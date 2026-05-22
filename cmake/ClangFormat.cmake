# SPDX-License-Identifier: Apache-2.0
#
# Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
# All rights reserved.
#
# Adds optional code-formatting targets:
#
#   cmake --build build --target format         # rewrite in place
#   cmake --build build --target format-check   # exit non-zero on diff
#
# Also registers a `nsgai_format_check` ctest test so `ctest` catches
# formatting drift.  If `clang-format` is not on PATH, the targets and
# the test are silently skipped (so packaging / minimal-deps builds
# are not broken).

find_program(CLANG_FORMAT_EXECUTABLE
    NAMES clang-format-22 clang-format-21 clang-format-20 clang-format
    DOC "Path to clang-format")

if(NOT CLANG_FORMAT_EXECUTABLE)
    message(STATUS "clang-format not found - 'format' targets disabled")
    return()
endif()

# Collect all first-party C sources + headers.  Globs are intentional
# here (vs. listing files) so a new source automatically shows up.
file(GLOB_RECURSE _nsgai_format_files CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/include/*.h"
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/examples/*.c"
    "${CMAKE_SOURCE_DIR}/examples/*.h"
    "${CMAKE_SOURCE_DIR}/test/*.c"
    "${CMAKE_SOURCE_DIR}/test/*.h"
)

add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXECUTABLE} -i --style=file ${_nsgai_format_files}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Reformatting C sources with clang-format"
    VERBATIM
)

add_custom_target(format-check
    COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run -Werror --style=file
            ${_nsgai_format_files}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking C source formatting with clang-format"
    VERBATIM
)

if(NSGAI_BUILD_TESTS)
    add_test(NAME nsgai_format_check
             COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run -Werror
                     --style=file ${_nsgai_format_files}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
