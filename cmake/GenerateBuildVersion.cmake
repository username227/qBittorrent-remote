cmake_minimum_required(VERSION 3.21)
# SPDX-License-Identifier: GPL-3.0-or-later
set(revision "")
set(release "${RELEASE_BUILD}")
if(GIT_EXECUTABLE AND EXISTS "${GIT_EXECUTABLE}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${REPO_DIR}"
        OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE git_result ERROR_QUIET)
    if(NOT git_result EQUAL 0 OR NOT revision MATCHES "^[0-9a-f]+$")
        set(revision "")
    else()
        execute_process(COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD
            WORKING_DIRECTORY "${REPO_DIR}"
            OUTPUT_VARIABLE tags OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(REPLACE "\n" ";" tags "${tags}")
        if("v${VERSION}" IN_LIST tags OR "${VERSION}" IN_LIST tags)
            set(release TRUE)
        endif()
    endif()
endif()
if(release)
    set(display "${VERSION}")
elseif(revision)
    set(display "${VERSION}-dev (${revision})")
else()
    set(display "${VERSION}-dev (revision unavailable)")
endif()
get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(CONFIGURE OUTPUT "${OUTPUT}" CONTENT
"#pragma once\n#define QBT_REMOTE_BUILD_VERSION \"@VERSION@\"\n#define QBT_REMOTE_DISPLAY_VERSION \"@display@\"\n" @ONLY)
