# SPDX-License-Identifier: Apache-2.0
# r16 D12 运行时闸 ztest（冷却/频度/风暴/fail-closed——owner v1 阈值）。
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_td12)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
zephyr_include_directories(${ENTELEA_ROOT}/framework/include)
target_compile_definitions(app PRIVATE ENTELEA_TEST=1)
target_sources(app PRIVATE
  src/main.c
  ${ENTELEA_ROOT}/framework/services/config/d12.c
  ${ENTELEA_ROOT}/framework/common/time.c
)
