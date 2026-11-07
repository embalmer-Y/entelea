# SPDX-License-Identifier: Apache-2.0
find_program(boot_trace_patch_tool patch REQUIRED)
get_target_property(boot_trace_sources zephyr SOURCES)
set(boot_trace_original "")
foreach(source IN LISTS boot_trace_sources)
  if(source MATCHES "/zephyr/esp32p4/src/soc_init.c$")
    if(boot_trace_original)
      message(FATAL_ERROR "Multiple P4 clock initialization sources")
    endif()
    set(boot_trace_original "${source}")
  endif()
endforeach()
if(NOT boot_trace_original)
  message(FATAL_ERROR "P4 clock initialization source not found")
endif()
file(SHA256 "${boot_trace_original}" boot_trace_hash)
if(NOT boot_trace_hash STREQUAL
    "13bcc5caa3ea92669340acc69ee1aab5d2b79aec1ca8c782accc15a3e41aa71e")
  message(FATAL_ERROR "Boot trace requires the exact unmodified pinned P4 clock source")
endif()

set(boot_trace_directory "${CMAKE_CURRENT_BINARY_DIR}/boot-trace-hal")
file(MAKE_DIRECTORY "${boot_trace_directory}")
configure_file("${boot_trace_original}" "${boot_trace_directory}/soc_init.c" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/clock_trace.patch")
execute_process(
  COMMAND "${boot_trace_patch_tool}" --batch --fuzz=0 --no-backup-if-mismatch
    -p1 -i "${CMAKE_CURRENT_LIST_DIR}/clock_trace.patch"
  WORKING_DIRECTORY "${boot_trace_directory}"
  RESULT_VARIABLE boot_trace_patch_result
)
if(NOT boot_trace_patch_result EQUAL 0)
  message(FATAL_ERROR "Cannot instrument the build-local P4 clock source")
endif()
list(REMOVE_ITEM boot_trace_sources "${boot_trace_original}")
set_property(TARGET zephyr PROPERTY SOURCES "${boot_trace_sources}")
target_sources(zephyr PRIVATE "${boot_trace_directory}/soc_init.c")
target_include_directories(zephyr PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
