# SPDX-License-Identifier: GPL-3.0-or-later
# Registration integrity check, not a substitute for any functional test.
execute_process(
    COMMAND "${TEST_EXECUTABLE}" -functions
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 10
)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "QtTest function discovery failed (${result}): ${error}")
endif()

string(REPLACE "\r\n" "\n" output "${output}")
string(REPLACE "\n" ";" lines "${output}")
set(discovered "")
foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)\\(\\)$")
        list(APPEND discovered "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT discovered)
    message(FATAL_ERROR "QtTest returned no discoverable workflow functions: ${output}")
endif()

set(duplicates "${REGISTERED_METHODS}")
list(REMOVE_DUPLICATES duplicates)
list(LENGTH duplicates unique_count)
list(LENGTH REGISTERED_METHODS registered_count)
if(NOT unique_count EQUAL registered_count)
    message(FATAL_ERROR "A remote workflow function is registered more than once.")
endif()

set(missing "")
set(unknown "")
foreach(method IN LISTS discovered)
    if(NOT method IN_LIST REGISTERED_METHODS)
        list(APPEND missing "${method}")
    endif()
endforeach()
foreach(method IN LISTS REGISTERED_METHODS)
    if(NOT method IN_LIST discovered)
        list(APPEND unknown "${method}")
    endif()
endforeach()
if(missing OR unknown)
    message(FATAL_ERROR
        "Remote workflow registration mismatch. Unregistered: [${missing}]; unknown: [${unknown}]")
endif()
message(STATUS "All ${registered_count} authenticated workflow functions are registered exactly once.")
