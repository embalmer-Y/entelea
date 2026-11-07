# SPDX-License-Identifier: Apache-2.0
find_program(gdma_patch_tool patch REQUIRED)
get_target_property(gdma_sources drivers__dma SOURCES)
set(gdma_original "")
foreach(source IN LISTS gdma_sources)
  if(source MATCHES "(^|/)dma_esp32_gdma[.]c$")
    if(gdma_original)
      message(FATAL_ERROR "Multiple ESP32 GDMA sources")
    endif()
    set(gdma_original "${source}")
  endif()
endforeach()
if(NOT gdma_original)
  message(FATAL_ERROR "ESP32 GDMA source not found")
endif()
get_filename_component(gdma_absolute "${gdma_original}" ABSOLUTE
  BASE_DIR "${ZEPHYR_BASE}/drivers/dma")
file(SHA256 "${gdma_absolute}" gdma_original_hash)
if(NOT gdma_original_hash STREQUAL
    "91dec7e51edc904f542dd18d8a9795aaac7228764508c0c916692e15204e438d")
  message(FATAL_ERROR "I2S comparison requires the unmodified pinned GDMA source")
endif()

set(gdma_patch "${CMAKE_CURRENT_LIST_DIR}/../../../../patches/zephyr-esp32-gdma.patch")
file(SHA256 "${gdma_patch}" gdma_patch_hash)
if(NOT gdma_patch_hash STREQUAL
    "8dd2636f67b0a6688fd6d86660401be14262411635cf2965270230cbc7696a46")
  message(FATAL_ERROR "The archived GDMA candidate has changed")
endif()
set(gdma_directory "${CMAKE_CURRENT_BINARY_DIR}/gdma-candidate")
file(MAKE_DIRECTORY "${gdma_directory}/drivers/dma")
configure_file("${gdma_absolute}"
  "${gdma_directory}/drivers/dma/dma_esp32_gdma.c" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${gdma_patch}")
execute_process(
  COMMAND "${gdma_patch_tool}" --batch --fuzz=0 --no-backup-if-mismatch
    -p1 -i "${gdma_patch}"
  WORKING_DIRECTORY "${gdma_directory}"
  RESULT_VARIABLE gdma_patch_result
)
if(NOT gdma_patch_result EQUAL 0)
  message(FATAL_ERROR "Cannot apply the isolated GDMA candidate")
endif()
list(REMOVE_ITEM gdma_sources "${gdma_original}")
set_property(TARGET drivers__dma PROPERTY SOURCES "${gdma_sources}")
target_sources(drivers__dma PRIVATE "${gdma_directory}/drivers/dma/dma_esp32_gdma.c")
target_include_directories(drivers__dma PRIVATE "${ZEPHYR_BASE}/drivers/dma")
file(SHA256 "${gdma_directory}/drivers/dma/dma_esp32_gdma.c" gdma_candidate_hash)
message(STATUS "Isolated P4 GDMA source SHA256: ${gdma_candidate_hash}")
