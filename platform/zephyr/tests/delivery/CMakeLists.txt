# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_tests_delivery)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
zephyr_include_directories(${ENTELEA_ROOT}/framework/include
                           ${ENTELEA_ROOT}/platform/zephyr
                           ${ENTELEA_ROOT}/framework/services/package)
target_sources(app PRIVATE
  src/main.c
  ${ENTELEA_ROOT}/framework/services/delivery/delivery.c
  # A-4 持久面（meta 经存储端口；RAM 后端即测语义）。
  ${ENTELEA_ROOT}/framework/services/storage/part.c
  ${ENTELEA_ROOT}/framework/services/storage/meta.c
  ${ENTELEA_ROOT}/framework/services/storage/noinit.c
  ${ENTELEA_ROOT}/framework/services/storage/audit.c
  ${ENTELEA_ROOT}/framework/services/storage/trust.c
  ${ENTELEA_ROOT}/framework/services/storage/kv.c
  ${ENTELEA_ROOT}/framework/services/config/prov.c
  ${ENTELEA_ROOT}/framework/services/config/prov_maint.c
  # F4：候选摘要由设备经验签后的整文件摘要计算（store.c）。
  ${ENTELEA_ROOT}/framework/services/package/store.c
  ${ENTELEA_ROOT}/framework/services/package/crypto/tweetnacl.c
  # 权限列表文法（与 wasm 宿主共用，perm.c 的拒绝留痕依赖 events/time）。
  ${ENTELEA_ROOT}/framework/execution/capability/perm.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/events.c
  ${ENTELEA_ROOT}/framework/common/time.c
)
target_compile_definitions(app PRIVATE ENTELEA_TEST=1)
# tweetnacl 上游致密风格（PROVENANCE 登记的第三方警告基线豁免）。
set_source_files_properties(${ENTELEA_ROOT}/framework/services/package/crypto/tweetnacl.c
  PROPERTIES COMPILE_OPTIONS "-w")
