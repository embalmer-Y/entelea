# SPDX-License-Identifier: Apache-2.0
"""Exercise the actual optional boot wrappers, without simulating P4 clocks."""

import hashlib
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "firmware/tests/board/p4touchid/src/boot_trace.c"
PINNED_CLOCK_HASH = "13bcc5caa3ea92669340acc69ee1aab5d2b79aec1ca8c782accc15a3e41aa71e"
HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/arch/riscv/exception.h>

int __wrap_hardware_init(void);
void __wrap_soc_hw_init(void);
void __wrap_ana_reset_config(void);
void __wrap_super_wdt_auto_feed(void);
void __wrap_bootloader_clock_configure(void);
void __wrap_rtc_clk_8m_enable(bool enable);
void __wrap_esp_rom_delay_us(uint32_t delay);
uint32_t __wrap_get_act_hp_dbias(void);
uint32_t __wrap_get_act_lp_dbias(void);
void __wrap_regi2c_exit_critical(void);
void __wrap_z_riscv_fault(struct arch_esf *esf);
void __wrap_z_fatal_error(unsigned int reason, const struct arch_esf *esf);
void ts_p4_clock_checkpoint(char stage, char edge);
void ts_p4_clock_poll(void);
int ts_p4_boot_soft_resets(void);

uint32_t fake_wdt[3] = { 0x4000, 0x10000, 26000000 };
static int result;
static unsigned int delays;
static unsigned int soc_calls;
static unsigned int reset_calls;
static unsigned int wdt_calls;
static unsigned int clock_calls;
static unsigned int rc_calls;
static unsigned int unlocks;
static int reset_reason = 1;
static const struct arch_esf *expected_esf;
static jmp_buf reset_env;
static unsigned int soft_calls;
static bool reset_returns;

int esp_rom_printf(const char *format, ...)
{
	va_list args;
	int ret;

	va_start(args, format);
	ret = vprintf(format, args);
	va_end(args);
	return ret;
}

void esp_rom_output_tx_wait_idle(uint8_t port) { assert(port == 0); }
int esp_rom_get_reset_reason(int cpu) { assert(cpu == 0); return reset_reason; }
void esp_rom_software_reset_cpu(int cpu)
{
	assert(cpu == 0);
	reset_reason = 12;
	soft_calls++;
	if (!reset_returns) {
		longjmp(reset_env, 1);
	}
}
void __real_z_riscv_fault(struct arch_esf *esf) { assert(esf == expected_esf); }
void __real_z_fatal_error(unsigned int reason, const struct arch_esf *esf)
{
	assert(reason == 3 && esf == expected_esf);
}
void __real_soc_hw_init(void) { soc_calls++; }
void __real_ana_reset_config(void) { reset_calls++; }
void __real_super_wdt_auto_feed(void) { wdt_calls++; }
void __real_rtc_clk_8m_enable(bool enable) { assert(enable); rc_calls++; }
void __real_regi2c_exit_critical(void) { unlocks++; }
uint32_t __real_get_act_hp_dbias(void) { return 0x24; }
uint32_t __real_get_act_lp_dbias(void) { return 0x12; }
void __real_esp_rom_delay_us(uint32_t delay)
{
	assert(delay == 10 || delay == 17 || delay == 29);
	delays += delay;
}

void __real_bootloader_clock_configure(void)
{
	clock_calls++;
	ts_p4_clock_checkpoint('Q', '+');
	ts_p4_clock_poll();
	ts_p4_clock_poll();
	ts_p4_clock_poll();
	ts_p4_clock_checkpoint('Q', '-');
	__wrap_rtc_clk_8m_enable(true);
	__wrap_regi2c_exit_critical();
	__wrap_esp_rom_delay_us(10);
	assert(__wrap_get_act_hp_dbias() == 0x24);
	assert(__wrap_get_act_lp_dbias() == 0x12);
	__wrap_esp_rom_delay_us(17);
	ts_p4_clock_checkpoint('M', '-');
}

int __real_hardware_init(void)
{
	__wrap_soc_hw_init();
	__wrap_ana_reset_config();
	__wrap_super_wdt_auto_feed();
	__wrap_bootloader_clock_configure();
	fake_wdt[0] &= ~0x4000;
	return result;
}

int main(int argc, char **argv)
{
	struct arch_esf frame = { .ra = 0x44, .a0 = 1000, .mstatus = 0x88 };

	assert(argc == 3);
	result = atoi(argv[1]);
	reset_reason = atoi(argv[2]);
	__wrap_esp_rom_delay_us(29);
	ts_p4_clock_checkpoint('!', '+');
	ts_p4_clock_poll();
	assert(__wrap_hardware_init() == result);
	__wrap_esp_rom_delay_us(29);
	fake_wdt[0] = 0x4000;
	assert(__wrap_hardware_init() == result);
	ts_p4_clock_checkpoint('!', '-');
	ts_p4_clock_poll();
	assert(delays == 112);
	assert(soc_calls == 2 && reset_calls == 2 && wdt_calls == 2);
	assert(clock_calls == 2 && rc_calls == 2 && unlocks == 2);
	__wrap_z_riscv_fault(NULL);
	__wrap_z_fatal_error(3, NULL);
	expected_esf = &frame;
	__wrap_z_riscv_fault(&frame);
	__wrap_z_fatal_error(3, &frame);
	reset_reason = 12;
	assert(ts_p4_boot_soft_resets() == -EIO);
	reset_reason = 1;
	while (soft_calls < 5) {
		if (setjmp(reset_env) == 0) {
			ts_p4_boot_soft_resets();
			assert(false);
		}
	}
	assert(ts_p4_boot_soft_resets() == 0);
	reset_reason = 7;
	assert(ts_p4_boot_soft_resets() == -EIO);
	reset_reason = 1;
	reset_returns = true;
	assert(ts_p4_boot_soft_resets() == -EIO);
	assert(soft_calls == 6);
	return 0;
}
"""


@pytest.fixture(scope="module", params=[False, True])
def boot_trace_binary(tmp_path_factory, request):
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("Host C compiler unavailable")
    folder = tmp_path_factory.mktemp("boot-trace")
    (folder / "esp_attr.h").write_text(
        "#define IRAM_ATTR\n#define DRAM_STR(value) (value)\n#define RTC_NOINIT_ATTR\n"
    )
    (folder / "esp_rom_serial_output.h").write_text(
        "#include <stdint.h>\nvoid esp_rom_output_tx_wait_idle(uint8_t port);\n"
    )
    (folder / "esp_rom_sys.h").write_text(
        "int esp_rom_printf(const char *format, ...);\nvoid esp_rom_software_reset_cpu(int);\n"
    )
    (folder / "soc").mkdir()
    (folder / "zephyr/arch/riscv").mkdir(parents=True)
    (folder / "zephyr/arch/riscv/csr.h").write_text("#define csr_read(reg) 0u\n")
    (folder / "zephyr/arch/riscv/exception.h").write_text(
        "struct arch_esf { unsigned long ra, a0, mstatus; };\n"
    )
    (folder / "soc/soc.h").write_text(
        "#define REG_READ(address) (*(uint32_t *)(uintptr_t)(address))\n"
        "#define REG_WRITE(address, value) (*(uint32_t *)(uintptr_t)(address) = (value))\n"
    )
    (folder / "soc/timer_group_reg.h").write_text(
        "#include <stdint.h>\nextern uint32_t fake_wdt[3];\n"
        "#define TIMG_WDTCONFIG0_REG(i) ((uintptr_t)&fake_wdt[0])\n"
        "#define TIMG_WDTCONFIG1_REG(i) ((uintptr_t)&fake_wdt[1])\n"
        "#define TIMG_WDTCONFIG2_REG(i) ((uintptr_t)&fake_wdt[2])\n"
    )
    (folder / "soc/rtc.h").write_text(
        "#define RESET_REASON_CORE_MWDT 7\n#define RESET_REASON_CORE_RWDT 9\n"
        "#define RESET_REASON_CPU_MWDT 11\n#define RESET_REASON_CPU_RWDT 13\n"
        "#define RESET_REASON_CHIP_POWER_ON 1\n#define RESET_REASON_CPU_SW 12\n"
        "#define RESET_REASON_SYS_RWDT 16\nint esp_rom_get_reset_reason(int cpu);\n"
    )
    harness = folder / "harness.c"
    harness.write_text(HARNESS)
    binary = folder / "boot-trace"
    flags = ["-DCONFIG_TS_P4_BOOT_TRACE_DETAILS=1"] if request.param else []
    subprocess.run(
        [
            compiler,
            *flags,
            "-DCONFIG_TS_P4_BOOT_SOFT_RESETS=1",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(folder),
            str(SOURCE),
            str(harness),
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
    )
    return binary, request.param


@pytest.mark.parametrize("result", [0, -5])
@pytest.mark.parametrize("reset_reason", [0, 1, 7, 9, 11, 13, 16])
def test_real_wrappers_preserve_order_arguments_results_and_clock_scope(
    boot_trace_binary, result, reset_reason
):
    binary, details = boot_trace_binary
    run = subprocess.run(
        [str(binary), str(result), str(reset_reason)],
        capture_output=True,
        text=True,
        timeout=5,
    )
    assert run.returncode == 0, run.stderr
    expected = ["H+", "S+", "S-", "A+", "A-", "W+", "W-", "C+"]
    if details:
        for stage in ("R", "E", "D", "P", "L", "D"):
            expected.extend((stage + "+", stage + "-"))
    expected.extend(("C-", "H-"))
    expected = ["@" + stage for stage in expected]
    fine = "@FINE=M- polls=3"
    wdt_entry = "@WDT0 at=entry config0=00004000 config1=00010000 hold=26000000"
    expected += [
        fine,
        wdt_entry,
        "@WDT0 at=after config0=00000000 config1=00010000 hold=26000000",
    ]
    second_boot = expected.copy()
    if reset_reason in (7, 9, 11, 13, 16):
        expected[1:1] = ["@LAST=unavailable"]
        second_boot[1:1] = ["@LAST=C- delay_index=2 delay_us=17", fine, wdt_entry]
    assert run.stdout.splitlines() == expected + second_boot + [
        "@FAULT cause=00000000 pc=00000000 value=00000000",
        "@CONTEXT unavailable",
        "@CLOCK checkpoint=b004432d delay_index=2 delay_us=17",
        fine,
        wdt_entry,
        "@FATAL reason=3 pc=00000000",
        "@FAULT cause=00000000 pc=00000000 value=00000000",
        "@CONTEXT a0=000003e8 ra=00000044 status=00000088",
        "@CLOCK checkpoint=b004432d delay_index=2 delay_us=17",
        fine,
        wdt_entry,
        "@FATAL reason=3 pc=00000000",
        "P4SOFT ABORT reset=12 state=00000000",
        *(f"P4SOFT REQUEST index={i}" for i in range(1, 6)),
        "P4SOFT DONE count=5",
        "P4SOFT ABORT reset=7 state=b0050005",
        "P4SOFT REQUEST index=1",
        "P4SOFT FAIL reset_returned",
    ]


@pytest.mark.parametrize("mode", ("ok", "changed", "missing", "duplicate", "patch-error"))
def test_build_local_clock_trace(tmp_path, mode):
    cmake = shutil.which("cmake")
    pinned = ROOT / ".deps/hal_espressif/zephyr/esp32p4/src/soc_init.c"
    if not cmake or not pinned.is_file():
        pytest.skip("CMake and the optional pinned P4 HAL are required")
    original = pinned.read_bytes()
    if hashlib.sha256(original).hexdigest() != PINNED_CLOCK_HASH:
        pytest.skip("The optional P4 HAL is not the fixed trace input")
    app = SOURCE.parents[1]
    module = tmp_path / "module"
    module.mkdir()
    shutil.copyfile(app / "clock_trace.cmake", module / "clock_trace.cmake")
    patch = module / "clock_trace.patch"
    shutil.copyfile(app / "clock_trace.patch", patch)
    if mode == "patch-error":
        patch.write_text("invalid diagnostic patch\n")
    source = tmp_path / "hal/zephyr/esp32p4/src/soc_init.c"
    source.parent.mkdir(parents=True)
    source.write_bytes(original + (b"\n" if mode == "changed" else b""))
    before = source.read_bytes()
    sources = [source]
    if mode == "missing":
        dummy = tmp_path / "dummy.c"
        dummy.write_text("int dummy;\n")
        sources = [dummy]
    elif mode == "duplicate":
        sources *= 2
    (tmp_path / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\nproject(trace_test C)\n"
        f"add_library(zephyr OBJECT {' '.join(str(path) for path in sources)})\n"
        f'include("{module / "clock_trace.cmake"}")\n'
        "get_target_property(active_sources zephyr SOURCES)\n"
        'file(WRITE "${CMAKE_BINARY_DIR}/active-sources" "${active_sources}")\n'
    )
    build = tmp_path / "build"
    command = [cmake, "-S", str(tmp_path), "-B", str(build)]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    assert pinned.read_bytes() == original
    assert source.read_bytes() == before
    if mode != "ok":
        errors = {
            "changed": "exact unmodified pinned",
            "missing": "source not found",
            "duplicate": "Multiple P4 clock",
            "patch-error": "Cannot instrument",
        }
        assert result.returncode != 0
        assert errors[mode] in result.stdout + result.stderr
        return
    assert result.returncode == 0, result.stdout + result.stderr
    generated = build / "boot-trace-hal/soc_init.c"
    first = generated.read_bytes()
    active = (build / "active-sources").read_text().split(";")
    assert active == [str(generated)]
    assert "ts_p4_clock_checkpoint('Q', '+')" in first.decode()
    subprocess.run(command, capture_output=True, text=True, check=True)
    assert generated.read_bytes() == first
    assert source.read_bytes() == before
    subprocess.run(
        [shutil.which("patch") or "patch", "-R", "--batch", "--fuzz=0", "-p1", "-i", str(patch)],
        cwd=generated.parent,
        capture_output=True,
        text=True,
        check=True,
    )
    assert generated.read_bytes() == original


def test_clock_patch_only_adds_observation():
    lines = (SOURCE.parents[1] / "clock_trace.patch").read_text().splitlines()
    added = [line[1:] for line in lines if line.startswith("+") and not line.startswith("+++")]
    removed = [line[1:] for line in lines if line.startswith("-") and not line.startswith("---")]
    assert removed == ["\t\t;"]
    assert added[0] == '#include "boot_trace.h"'
    assert added.count("\t\tts_p4_clock_poll();") == 1
    expected = [
        f"\tts_p4_clock_checkpoint('{stage}', '{edge}');"
        for stage, edge in [
            ("B", "+"),
            ("Q", "+"),
            ("Q", "-"),
            ("B", "-"),
            *((stage, edge) for stage in "VOasmcFM" for edge in "+-"),
        ]
    ]
    assert [line for line in added[1:] if line != "\t\tts_p4_clock_poll();"] == expected
