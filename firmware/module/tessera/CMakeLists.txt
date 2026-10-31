# SPDX-License-Identifier: Apache-2.0
# Tessera framework module root.
# 子系统按 design/LLD-* 加入：M1 = ts-core + ts-safety；M2/M3 追加 hal/appmgr/
# net/power/periph/store（各 LLD）。
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

zephyr_library()

zephyr_library_sources(
  # cbor_min = 模块内共享最小 CBOR 编解码（net 命令面 + appmgr boot_start
  # manifest 走查共用——板级五实证 TS_NET=n 时 appmgr 亦依赖，故无条件编译）
  src/net/cbor_min.c
)

zephyr_library_sources_ifdef(CONFIG_TS_CORE
  src/core/time.c
  src/core/events.c
  src/core/wdt.c
  src/core/boot.c
)

zephyr_library_sources_ifdef(CONFIG_TS_SAFETY
  src/safety/channel.c
  src/safety/commit.c
  src/safety/force.c
  src/safety/driver_dispatch.c
)

zephyr_library_sources_ifdef(CONFIG_TS_STORE
  src/store/part.c
  src/store/meta.c
  src/store/prov.c
  src/store/noinit.c
  src/store/slot.c
  src/store/sha256.c
)

zephyr_library_sources_ifdef(CONFIG_TS_STORE_AUDIT
  src/store/audit.c
)

zephyr_library_sources_ifdef(CONFIG_TS_STORE_KV
  src/store/kv.c
)

zephyr_library_sources_ifdef(CONFIG_TS_STORE_TRUST
  src/store/ts_trust.c
)

zephyr_library_sources_ifdef(CONFIG_TS_DRV_PDM
  src/hal/pdm.c
)

zephyr_library_sources_ifdef(CONFIG_TS_DRV_ES8311
  src/hal/es8311.c
)

zephyr_library_sources_ifdef(CONFIG_TS_STORE
  src/store/prov_maint.c
)

zephyr_library_sources_ifdef(CONFIG_TS_TEST
  src/store/prov_test.c
)

zephyr_library_sources_ifdef(CONFIG_TS_INTERACT
  src/interact/ts_interact.c
)

# v4.5 适配（升级批教训 30③，P4 无线面批扩为模块级）：4.5 移除 legacy 生成
# 头路径——WAMR 平件 platform_internal.h 裸 include <autoconf.h>（上游
# 2.4.5）与 zenoh-pico 平台头 zephyr.h 裸 include <version.h>（1.10.1，做
# 内核版本分派）同族。以生成头新路径（<generated>/zephyr/）补兼容包含域
# ——仓库内修复（CI 可复现），零上游补丁纪律不破。原 WAMR 条件块内的
# 同名语句已并入本处（TS_APP_WAMR=n 的 netbench 面亦依赖——P4 无线批
# 实证）。
zephyr_include_directories(${ZEPHYR_BINARY_DIR}/include/generated/zephyr)


zephyr_library_sources_ifdef(CONFIG_TS_HAL
  src/hal/perm.c
  src/hal/api.c
  src/hal/input.c

)

zephyr_library_sources_ifdef(CONFIG_TS_APPMGR
  src/appmgr/pkg.c
  src/appmgr/slot.c
  src/appmgr/verify.c
  src/appmgr/crypto/tweetnacl.c
  src/appmgr/crypto/tnacl_selftest.c
)

# tweetnacl（三方公版，crypto/PROVENANCE.md）：致密风格代码不适用本项目
# 警告基线——单独降为 -w（其余编译选项不变）
set_source_files_properties(src/appmgr/crypto/tweetnacl.c
  PROPERTIES COMPILE_OPTIONS "-w")

zephyr_library_sources_ifdef(CONFIG_TS_APP_WAMR
  src/appmgr/runtime.c
  src/appmgr/natives.c
)

zephyr_library_sources_ifdef(CONFIG_TS_NET
  src/net/keyspace.c
  src/net/pubq.c
  src/net/avq.c
  src/net/session.c
  src/net/linkmon.c
  src/net/cmd.c
  src/net/lease.c
  src/net/pub.c
  src/net/init.c
)

zephyr_library_sources_ifdef(CONFIG_TS_NET_ZENOH
  src/net/zenoh.c
)

zephyr_library_sources_ifdef(CONFIG_TS_POWER
  src/power/slots.c
  src/power/budget.c
)

zephyr_library_sources_ifdef(CONFIG_TS_HAL_FS
  src/hal/fs.c
)

zephyr_library_sources_ifdef(CONFIG_TS_HAL_AV
  src/hal/av.c
)

zephyr_library_sources_ifdef(CONFIG_TS_PERIPH
  src/periph/desc.c
  src/periph/hotplug.c
)

# ---- WAMR 运行时（M2b.2a 环境批；CONFIG_TS_APP_WAMR 门控）------------------
# DEC-17（WAMR）/ DEC-25（fast 解释器 + WASI 全关 + AOT 不入固件构建）/
# DEC-31（线程/共享内存特性编译期不启用）。源码根经环境变量 TS_WAMR_DIR 注入
# （本地 = ~/project/deps/wamr，CI 检出步骤注入；仓库不含三方源码——与
# zenoh-pico 同纪律）。板级目标随移植批映射（ESP32-S3 = XTENSA）。
if(CONFIG_TS_APP_WAMR)
  if(NOT DEFINED ENV{TS_WAMR_DIR})
    message(FATAL_ERROR
      "TS_WAMR_DIR not set (WAMR source root; local: ~/project/deps/wamr, CI: checkout step)")
  endif()
  set(WAMR_BUILD_PLATFORM "zephyr")
  # 目标架构按板映射（板级移植批）：native_sim = X86_32（32 位基线）；
  # xiao_esp32s3 = XTENSA（LX7）。新板须显式登记映射，防默认静默错配。
  if(BOARD STREQUAL "native_sim")
    # 64 位变体（native_sim/native/64——ARM 机 twister 口径）：宿主字长目标，
    # 按宿主处理器选（native 工具链 = 宿主本机编译）。32 位基线维持 X86_32。
    if(CONFIG_64BIT OR BOARD_QUALIFIERS MATCHES "64")
      # R3（DEC-64④）根治：此前 /64 统一 X86_64 + GENERAL C thunk（诚实崩
      # 溃映射）——根因 = GENERAL thunk 契约是「argc 个连续 u32 字」（32 位
      # branch A 线性布局），而 64 位目标编译寄存器分类路径（fps/ints/stacks
      # 三桶，exec_env 64 位值进 ints[0]，小签名 n_stacks=0）→ GENERAL 收
      # argc=0 零参数调用 native → exec_env 形参 = 寄存器残留 → user_data
      # = 常数垃圾 0xaa0803f0a9467bfd → SIGSEGV（arm64_rebuild §7 活标本）。
      # 修复 = 64 位走真实架构汇编 thunk（寄存器分类布局的正牌消费者）：
      # 目标随宿主处理器映射；GENERAL=0。
      if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)")
        set(WAMR_BUILD_TARGET "AARCH64")
      else()
        set(WAMR_BUILD_TARGET "X86_64")
      endif()
      set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 0)
    else()
      set(WAMR_BUILD_TARGET "X86_32")
      # 通用 C 版 invokeNative：ia32 汇编缺 .note.GNU-stack（可执行栈警告被
      # Zephyr --fatal-warnings 升级为链接错误）；C 版零补丁且 32 位布局
      # （branch A 线性字数组）与其契约匹配——x86 CI 口径维持。
      set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 1)
    endif()
  elseif(BOARD STREQUAL "xiao_esp32s3")
    set(WAMR_BUILD_TARGET "XTENSA")
    # xtensa 用 WAMR 官方汇编 invokeNative（WAMR cmake 对 XTENSA 的既定
    # 路径；GENERAL C 版注释自认部分 CPU 传参不可靠）。汇编缺 GNU-stack
    # 注记 → 汇编期补（-Wa,--noexecstack）。
    set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 0)
  elseif(BOARD STREQUAL "dnesp32s3b")
    # ATK DNESP32-S3B（K1）：同为 ESP32-S3 XTENSA——xiao 同映射
    set(WAMR_BUILD_TARGET "XTENSA")
    set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 0)
  elseif(BOARD STREQUAL "esp32p4_wifi6_dev_kit"
         OR BOARD STREQUAL "esp32p4c5_core_board")
    # P4（P4 适配批）：HP 核 = RV32；解释器-only（AOT/JIT 关）下 target
    # 声明仅取 word 长/ABI 元数据，fast 解释器目标无关。invokeNative 不
    # 参与构建（AOT=0），GENERAL 旗标不设。esp32p4c5_core_board（P4C5
    # 适配批）同为 P4 HP 核 RV32。
    set(WAMR_BUILD_TARGET "RISCV32")
  elseif(BOARD STREQUAL "mini_stm32h743")
    # H7（H7 移植批）：Cortex-M7（Thumb-2）。target 取 THUMBV7EM 而非
    # ARM——WAMR 的 invokeNative_arm.s 是 A32 汇编，Zephyr ARM 全 -mthumb
    # 下汇编期即拒（lo register/寻址模式错误）；THUMB 分支选
    # invokeNative_thumb.s。解释器-only（AOT=0）下该汇编不参与运行，
    # 仅过链接（target 声明 = ABI 元数据）。
    set(WAMR_BUILD_TARGET "THUMBV7EM")
  else()
    message(FATAL_ERROR "WAMR_BUILD_TARGET unmapped for board '${BOARD}'")
  endif()
  set(WAMR_BUILD_INTERP 1)
  set(WAMR_BUILD_FAST_INTERP 1)       # DEC-25：fast 解释器
  set(WAMR_BUILD_AOT 0)               # DEC-25：AOT = Agent 构建期选项，不入固件
  set(WAMR_BUILD_INSTRUCTION_METERING 1) # DEC-48③（Q-28③A）：指令配额（超限异常
  # 走既有健康失败/回滚路径；上游开关零补丁）
  set(WAMR_BUILD_JIT 0)
  set(WAMR_BUILD_LIBC_BUILTIN 1)
  set(WAMR_BUILD_LIBC_WASI 0)         # DEC-25：WASI 全关
  set(WAMR_BUILD_MULTI_MODULE 0)
  # DEC-31 → DEC-55 修订（Q-37 A）：默认编译期不启用（基线构建/回归零
  # 扰动）；CONFIG_TS_APP_THREADS=y 显式启用（thread-mgr + 共享内存——
  # 宿主侧 spawn 编排，lib-pthread 仍不启用：其线程 env 计量初值 0 +
  # 计量宏 ==0 即异常 = 线程首指令死〔WAMR 2.4.5 上游已知不兼容面，
  # 实查定案〕；ts_thread_* natives 替代其语义）。
  set(WAMR_BUILD_LIB_PTHREAD 0)
  if(CONFIG_TS_APP_THREADS)
    set(WAMR_BUILD_SHARED_MEMORY 1)
    set(WAMR_BUILD_THREAD_MGR 1)
  else()
    set(WAMR_BUILD_SHARED_MEMORY 0)
  endif()
  set(WAMR_BUILD_GLOBAL_HEAP_POOL 1)  # 池模式（PSRAM 分层纪律，DEC-27）。
  # 注（板级六实证）：WAMR-2.4.5 中本旗标对 runtime 路径已无消费者——池
  # 语义实际由 runtime.c 经 wasm_runtime_full_init(Alloc_With_Pool, 注入堆
  # 缓冲) 显式建立（PSRAM = SMH 分配；其余 = 内部静态池）。保留定义不改
  # 上游口径。
  set(WAMR_BUILD_GLOBAL_HEAP_SIZE ${CONFIG_TS_APP_WAMR_HEAP})
  set(WAMR_ROOT_DIR $ENV{TS_WAMR_DIR})
  include($ENV{TS_WAMR_DIR}/build-scripts/runtime_lib.cmake)
  # 三方源码独立成库并 -w：钉版上游不裁剪告警（ems_gc.c 的 GB 常量与
  # Zephyr util.h 单位宏撞名等）——零上游补丁纪律（DR-22 教训同源）。
  zephyr_library_named(tessera_wamr)
  zephyr_library_sources(${WAMR_RUNTIME_LIB_SOURCE})
  zephyr_library_sources(src/appmgr/wamr_compat.c)
  zephyr_library_compile_options(-w)
  # xtensa/64 位 invokeNative 汇编缺 .note.GNU-stack → 汇编期补注记
  # （否则 Zephyr --fatal-warnings 链接失败；对 C 对象无副作用）。
  if(BOARD STREQUAL "xiao_esp32s3" OR BOARD STREQUAL "dnesp32s3b"
     OR (BOARD STREQUAL "native_sim" AND WAMR_BUILD_INVOKE_NATIVE_GENERAL EQUAL 0))
    zephyr_library_compile_options(-Wa,--noexecstack)
  endif()
  # 消费面（app/测试）include 传播：runtime_lib.cmake 内部为目录级
  # include_directories（仅本模块编译域可见）——Zephyr 下消费方须经
  # zephyr_include_directories 取到 wasm_export.h。
  zephyr_include_directories($ENV{TS_WAMR_DIR}/core/iwasm/include)
  # （v4.5 生成头包含域已提为模块级——见文件头部同名语句与注释。）
endif()
