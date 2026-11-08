# SPDX-License-Identifier: Apache-2.0

if(CONFIG_SOC_ESP32P4 AND CONFIG_ESP_SIMPLE_BOOT AND CONFIG_BUILD_OUTPUT_BIN)
  dt_nodelabel(entelea_boot_partition NODELABEL "boot_partition" REQUIRED)
  dt_chosen(entelea_code_partition PROPERTY "zephyr,code-partition")
  dt_reg_addr(entelea_boot_offset PATH "${entelea_boot_partition}")
  dt_reg_addr(entelea_code_offset PATH "${entelea_code_partition}")
  dt_reg_size(entelea_code_size PATH "${entelea_code_partition}")
  math(EXPR entelea_image_limit
       "${entelea_code_offset} + ${entelea_code_size} - ${entelea_boot_offset}")
  get_filename_component(entelea_p4_image_script
    "${CMAKE_CURRENT_LIST_DIR}/../../scripts/prepare_p4_image.py" ABSOLUTE)
  set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
    COMMAND "${PYTHON_EXECUTABLE}" "${entelea_p4_image_script}"
      "${PROJECT_BINARY_DIR}/${KERNEL_BIN_NAME}" --max-size "${entelea_image_limit}")
  set_property(TARGET zephyr_interface APPEND PROPERTY INTERFACE_LINK_DEPENDS
    "${entelea_p4_image_script}")
endif()
