# SPDX-License-Identifier: Apache-2.0
"""Entelea setup regressions and retained zenoh TLS patch integrity."""
from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[3]
ENV_DIR = REPO / "scripts"
TEST_WDT_PATCH = """diff --git a/subsys/task_wdt/task_wdt.c b/subsys/task_wdt/task_wdt.c
--- a/subsys/task_wdt/task_wdt.c
+++ b/subsys/task_wdt/task_wdt.c
@@ -1 +1 @@
-old
+new
"""
TEST_SCAN_PATCH = (
    "diff --git a/drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c "
    "b/drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c\n"
    """\
--- a/drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c
+++ b/drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c
@@ -1 +1 @@
-oldscan
+newscan
"""
)
TEST_DIGEST_PATCH = """diff --git a/soc/espressif/common/loader.c b/soc/espressif/common/loader.c
--- a/soc/espressif/common/loader.c
+++ b/soc/espressif/common/loader.c
@@ -1 +1 @@
-oldloader
+newloader
"""
TEST_GPIO_PATCH = """diff --git a/drivers/gpio/gpio_esp32.c b/drivers/gpio/gpio_esp32.c
--- a/drivers/gpio/gpio_esp32.c
+++ b/drivers/gpio/gpio_esp32.c
@@ -1 +1 @@
-oldgpio
+newgpio
"""
TEST_TOUCH_PATCH = """diff --git a/drivers/input/input_cst8xx.c b/drivers/input/input_cst8xx.c
--- a/drivers/input/input_cst8xx.c
+++ b/drivers/input/input_cst8xx.c
@@ -1 +1 @@
-oldtouch
+newtouch
"""
TEST_DISPLAY_PATCH = (
    "diff --git a/drivers/display/display_esp32_dsi.c "
    "b/drivers/display/display_esp32_dsi.c\n"
    """\
--- a/drivers/display/display_esp32_dsi.c
+++ b/drivers/display/display_esp32_dsi.c
@@ -1 +1 @@
-olddsi
+newdsi
"""
)


def _sh_files() -> list[Path]:
    return sorted(ENV_DIR.rglob("*.sh"))


def test_shell_syntax_all():
    files = _sh_files()
    assert {p.name for p in files} == {"setup-firmware.sh", "firmware.sh"}
    for f in files:
        r = subprocess.run(["bash", "-n", str(f)], capture_output=True, text=True)
        assert r.returncode == 0, f"{f}: {r.stderr}"


def test_setup_uses_immutable_revisions():
    src = (ENV_DIR / "setup-firmware.sh").read_text(encoding="utf-8")
    pins = re.findall(r'^\s+([0-9a-f]{40}) "\$ROOT/\.deps/([^"]+)"', src, re.M)
    assert len(pins) == 8
    assert {name for _, name in pins} == {
        "zephyr", "wamr", "hal_espressif", "nanopb",
        "mbedtls", "tf-psa-crypto", "tf-psa-crypto-drivers", "fatfs",
    }
    assert "sudo" not in src
    assert "reset --hard" not in src


def test_missing_sources_fail_explicitly(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    shutil.copyfile(ENV_DIR / "firmware.sh", scripts / "firmware.sh")
    result = subprocess.run(
        ["bash", str(scripts / "firmware.sh"), "build"],
        capture_output=True, text=True, timeout=10,
        env={"PATH": "/usr/bin:/bin", "HOME": str(tmp_path)},
    )
    assert result.returncode != 0
    assert "Missing source:" in result.stderr


@pytest.mark.parametrize(
    ("script", "args"),
    [
        ("firmware.sh", []),
        ("firmware.sh", ["invalid"]),
        ("firmware.sh", ["build", "extra"]),
        ("setup-firmware.sh", ["invalid"]),
        ("setup-firmware.sh", ["build", "extra"]),
    ],
)
def test_invalid_arguments_do_not_start_commands(tmp_path, script, args):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    shutil.copyfile(ENV_DIR / script, scripts / script)
    result = subprocess.run(
        ["bash", str(scripts / script), *args],
        capture_output=True, text=True, timeout=10,
        env={"PATH": "/usr/bin:/bin", "HOME": str(tmp_path)},
    )
    assert result.returncode == 2
    assert "Usage:" in result.stderr
    assert not (tmp_path / ".deps").exists()
    assert not (tmp_path / "build").exists()


@pytest.fixture
def firmware_workspace(tmp_path):
    (tmp_path / "scripts").mkdir()
    shutil.copyfile(ENV_DIR / "firmware.sh", tmp_path / "scripts/firmware.sh")
    for source in ("zephyr", "wamr"):
        path = tmp_path / ".deps" / source
        path.mkdir(parents=True)
        (path / "CMakeLists.txt").touch()
    (tmp_path / "patches").mkdir()
    (tmp_path / "patches/zephyr-task-wdt-setup.patch").write_text(TEST_WDT_PATCH)
    (tmp_path / "patches/zephyr-c5-scan.patch").write_text(TEST_SCAN_PATCH)
    (tmp_path / "patches/zephyr-p4-ram-digest.patch").write_text(TEST_DIGEST_PATCH)
    (tmp_path / "patches/zephyr-cst8xx-report.patch").write_text(TEST_TOUCH_PATCH)
    (tmp_path / "patches/zephyr-p4-dsi-frame-size.patch").write_text(TEST_DISPLAY_PATCH)
    (tmp_path / "patches/zephyr-esp32-gpio-base.patch").write_text(TEST_GPIO_PATCH)
    gpio = tmp_path / ".deps/zephyr/drivers/gpio/gpio_esp32.c"
    gpio.parent.mkdir(parents=True)
    gpio.write_text("newgpio\n")
    display = tmp_path / ".deps/zephyr/drivers/display/display_esp32_dsi.c"
    display.parent.mkdir(parents=True)
    display.write_text("newdsi\n")
    touch = tmp_path / ".deps/zephyr/drivers/input/input_cst8xx.c"
    touch.parent.mkdir(parents=True)
    touch.write_text("newtouch\n")
    source = tmp_path / ".deps/zephyr/subsys/task_wdt/task_wdt.c"
    source.parent.mkdir(parents=True)
    source.write_text("new\n")
    scan = tmp_path / ".deps/zephyr/drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c"
    scan.parent.mkdir(parents=True)
    scan.write_text("newscan\n")
    loader = tmp_path / ".deps/zephyr/soc/espressif/common/loader.c"
    loader.parent.mkdir(parents=True)
    loader.write_text("newloader\n")
    return tmp_path


def run_firmware(workspace, mode, **extra_env):
    return subprocess.run(
        ["bash", str(workspace / "scripts/firmware.sh"), mode],
        capture_output=True, text=True, timeout=10,
        env={"PATH": "/usr/bin:/bin", "HOME": str(workspace), **extra_env},
    )


@pytest.mark.parametrize(
    "mode", ["build-p4", "build-p4-store", "build-p4-net", "build-p4-tls",
             "build-p4-display", "build-p4-boot-trace", "build-p4-sd-detect",
             "build-p4-i2s", "build-p4-i2s-candidate", "build-p4-i2s-candidate32",
             "build-p4-i2s-stop", "build-p4-i2s-stop-candidate"]
)
def test_missing_p4_hal_fails_explicitly(firmware_workspace, mode):
    result = run_firmware(firmware_workspace, mode)
    assert result.returncode == 1
    assert "Missing P4 HAL:" in result.stderr
    assert not (firmware_workspace / "build").exists()


@pytest.mark.parametrize(
    "mode", ["build-p4", "build-p4-store", "build-p4-net", "build-p4-tls",
             "build-p4-display", "build-p4-boot-trace", "build-p4-sd-detect",
             "build-p4-i2s", "build-p4-i2s-candidate", "build-p4-i2s-candidate32",
             "build-p4-i2s-stop", "build-p4-i2s-stop-candidate"]
)
def test_missing_p4_sdk_fails_explicitly(firmware_workspace, mode):
    module = firmware_workspace / ".deps/hal_espressif/zephyr"
    module.mkdir(parents=True)
    (module / "module.yml").touch()
    result = run_firmware(firmware_workspace, mode)
    assert result.returncode == 1
    assert "ZEPHYR_SDK_INSTALL_DIR" in result.stderr
    assert not (firmware_workspace / "build").exists()


@pytest.mark.parametrize("mode", ["build-p4-net", "build-p4-tls"])
def test_missing_c5_nanopb_fails_before_build(firmware_workspace, mode):
    root = firmware_workspace
    (root / ".deps/hal_espressif/zephyr").mkdir(parents=True)
    (root / ".deps/hal_espressif/zephyr/module.yml").touch()
    sdk = root / "sdk"
    sdk.mkdir()
    (sdk / "sdk_version").touch()
    result = run_firmware(root, mode, ZEPHYR_SDK_INSTALL_DIR=str(sdk))
    assert result.returncode == 1
    assert "Missing C5 nanopb:" in result.stderr
    assert not (root / "build").exists()


def test_missing_sd_fatfs_fails_before_build(firmware_workspace):
    root = firmware_workspace
    (root / ".deps/hal_espressif/zephyr").mkdir(parents=True)
    (root / ".deps/hal_espressif/zephyr/module.yml").touch()
    sdk = root / "sdk"
    sdk.mkdir()
    (sdk / "sdk_version").touch()
    result = run_firmware(root, "build-p4-sd", ZEPHYR_SDK_INSTALL_DIR=str(sdk))
    assert result.returncode == 1
    assert "Missing FatFS module:" in result.stderr
    assert not (root / "build").exists()


def test_p4_sd_build_uses_only_approved_fatfs(firmware_workspace):
    root = firmware_workspace
    bin_dir = root / ".venv/bin"
    bin_dir.mkdir(parents=True)
    stub = bin_dir / "cmake"
    stub.write_text('#!/bin/bash\nprintf "%s\\n" "$@" >> "$HOME/commands"\n')
    stub.chmod(0o755)
    for name in ("hal_espressif", "fatfs"):
        module = root / ".deps" / name / "zephyr"
        module.mkdir(parents=True)
        (module / "module.yml").touch()
    sdk = root / "sdk"
    sdk.mkdir()
    (sdk / "sdk_version").touch()
    result = run_firmware(root, "build-p4-sd", ZEPHYR_SDK_INSTALL_DIR=str(sdk))
    assert result.returncode == 0, result.stderr
    commands = (root / "commands").read_text().splitlines()
    assert str(root / "firmware/tests/board/p4sdbench") in commands
    assert str(root / "build/firmware-p4-sd") in commands
    assert "-DKCONFIG_WARNING_AS_ERROR=ON" in commands
    assert (f"-DZEPHYR_MODULES={root}/firmware/module/tessera;"
            f"{root}/.deps/hal_espressif;{root}/.deps/fatfs") in commands
    assert not any(name in "\n".join(commands) for name in
                   ("nanopb", "mbedtls", "tf-psa-crypto"))


def test_p4_sd_detect_build_needs_no_filesystem_or_network_modules(firmware_workspace):
    root = firmware_workspace
    bin_dir = root / ".venv/bin"
    bin_dir.mkdir(parents=True)
    stub = bin_dir / "cmake"
    stub.write_text('#!/bin/bash\nprintf "%s\\n" "$@" >> "$HOME/commands"\n')
    stub.chmod(0o755)
    module = root / ".deps/hal_espressif/zephyr"
    module.mkdir(parents=True)
    (module / "module.yml").touch()
    sdk = root / "sdk"
    sdk.mkdir()
    (sdk / "sdk_version").touch()
    result = run_firmware(root, "build-p4-sd-detect", ZEPHYR_SDK_INSTALL_DIR=str(sdk))
    assert result.returncode == 0, result.stderr
    commands = (root / "commands").read_text().splitlines()
    assert str(root / "firmware/tests/board/p4sddetect") in commands
    assert str(root / "build/firmware-p4-sd-detect") in commands
    assert "-DKCONFIG_WARNING_AS_ERROR=ON" in commands
    assert (f"-DZEPHYR_MODULES={root}/firmware/module/tessera;"
            f"{root}/.deps/hal_espressif") in commands
    assert not any(name in "\n".join(commands) for name in
                   ("fatfs", "nanopb", "mbedtls", "tf-psa-crypto"))


@pytest.mark.parametrize(
    ("mode", "candidate", "word32", "lifecycle"),
    [
        ("build-p4-i2s", "OFF", "OFF", "OFF"),
        ("build-p4-i2s-candidate", "ON", "OFF", "OFF"),
        ("build-p4-i2s-candidate32", "ON", "ON", "OFF"),
        ("build-p4-i2s-stop", "OFF", "ON", "ON"),
        ("build-p4-i2s-stop-candidate", "ON", "ON", "ON"),
    ],
)
def test_p4_i2s_build_isolates_candidate(firmware_workspace, mode, candidate, word32, lifecycle):
    root = firmware_workspace
    bin_dir = root / ".venv/bin"
    bin_dir.mkdir(parents=True)
    stub = bin_dir / "cmake"
    stub.write_text('#!/bin/bash\nprintf "%s\\n" "$@" >> "$HOME/commands"\n')
    stub.chmod(0o755)
    module = root / ".deps/hal_espressif/zephyr"
    module.mkdir(parents=True)
    (module / "module.yml").touch()
    sdk = root / "sdk"
    sdk.mkdir()
    (sdk / "sdk_version").touch()
    result = run_firmware(root, mode, ZEPHYR_SDK_INSTALL_DIR=str(sdk))
    assert result.returncode == 0, result.stderr
    commands = (root / "commands").read_text().splitlines()
    assert str(root / "firmware/tests/board/p4i2sbench") in commands
    assert str(root / "build" / f"firmware-{mode.removeprefix('build-')}") in commands
    assert "-DKCONFIG_WARNING_AS_ERROR=ON" in commands
    assert f"-DP04_I2S_GDMA_CANDIDATE={candidate}" in commands
    assert f"-DP04_I2S_WORD32={word32}" in commands
    assert f"-DP04_I2S_LIFECYCLE_TEST={lifecycle}" in commands
    assert (f"-DZEPHYR_MODULES={root}/firmware/module/tessera;"
            f"{root}/.deps/hal_espressif") in commands
    assert not any(name in "\n".join(commands) for name in
                   ("fatfs", "nanopb", "mbedtls", "tf-psa-crypto"))


@pytest.mark.parametrize(
    "missing", ["mbedtls", "tf-psa-crypto", "tf-psa-crypto-drivers"]
)
def test_missing_tls_source_fails_before_build(firmware_workspace, missing):
    root = firmware_workspace
    for name in ("hal_espressif", "nanopb", "mbedtls", "tf-psa-crypto", "tf-psa-crypto-drivers"):
        if name == missing:
            continue
        module = root / ".deps" / name / "zephyr"
        module.mkdir(parents=True)
        (module / "module.yml").touch()
    sdk = root / "sdk"
    sdk.mkdir()
    (sdk / "sdk_version").touch()
    result = run_firmware(root, "build-p4-tls", ZEPHYR_SDK_INSTALL_DIR=str(sdk))
    assert result.returncode == 1
    assert f"Missing TLS module: {missing}" in result.stderr
    assert not (root / "build").exists()


@pytest.mark.parametrize(
    "mode", [
        "build", "test", "build-p4", "build-p4-store", "build-p4-net", "build-p4-tls",
        "build-p4-display", "build-p4-boot-trace", "build-p4-touch-id",
    ]
)
def test_build_modes_isolate_toolchain_and_preserve_module_wiring(firmware_workspace, mode):
    root = firmware_workspace
    bin_dir = root / ".venv/bin"
    bin_dir.mkdir(parents=True)
    for command in ("cmake", "python"):
        stub = bin_dir / command
        stub.write_text(
            '#!/bin/bash\nprintf "%s\\n" "$ZEPHYR_TOOLCHAIN_VARIANT" "$@" >> "$HOME/commands"\n',
            encoding="utf-8",
        )
        stub.chmod(0o755)
    hal = root / "external-hal"
    (hal / "zephyr").mkdir(parents=True)
    (hal / "zephyr/module.yml").touch()
    nanopb = root / "external-nanopb"
    (nanopb / "zephyr").mkdir(parents=True)
    (nanopb / "zephyr/module.yml").touch()
    tls_modules = []
    for name in ("mbedtls", "tf-psa-crypto", "tf-psa-crypto-drivers"):
        module = root / ".deps" / name
        (module / "zephyr").mkdir(parents=True)
        (module / "zephyr/module.yml").touch()
        tls_modules.append(str(module))
    sdk = root / "sdk"
    sdk.mkdir()
    (sdk / "sdk_version").write_text("1.0.1\n", encoding="utf-8")
    is_p4 = mode.startswith("build-p4")
    result = run_firmware(
        root, mode, ZEPHYR_TOOLCHAIN_VARIANT="host" if is_p4 else "zephyr",
        ZEPHYR_SDK_INSTALL_DIR=str(sdk), TS_HAL_ESPRESSIF_DIR=str(hal),
        TS_NANOPB_DIR=str(nanopb),
    )
    assert result.returncode == 0, result.stderr
    commands = (root / "commands").read_text().splitlines()
    expected = "zephyr" if is_p4 else "host"
    assert commands[0] == expected
    if is_p4:
        modules = f"-DZEPHYR_MODULES={root}/firmware/module/tessera;{hal}"
        if mode in ("build-p4-net", "build-p4-tls"):
            modules += f";{nanopb}"
        if mode == "build-p4-tls":
            modules += ";" + ";".join(tls_modules)
        assert modules in commands
        assert "-DBOARD=esp32p4c5_core_board/esp32p4/hpcore" in commands
        assert f"-DBOARD_ROOT={root}/firmware" in commands
        conf_arg = (
            f"-DEXTRA_CONF_FILE={root}/firmware/app/boards/"
            "esp32p4c5_core_board_esp32p4_hpcore.conf"
        )
        if mode == "build-p4-store":
            conf_arg += (
                f";{root}/firmware/app/boards/"
                "esp32p4c5_core_board_esp32p4_hpcore-store.conf"
            )
            assert (
                f"-DDTC_OVERLAY_FILE={root}/firmware/app/boards/"
                "esp32p4c5_core_board_esp32p4_hpcore-store.overlay"
            ) in commands
            assert str(root / "build/firmware-p4-store") in commands
        elif mode in ("build-p4-net", "build-p4-tls"):
            source = root / "firmware/tests/board/p4wifibench"
            assert str(source) in commands
            assert str(root / "firmware/app") not in commands
            build_dir = root / "build" / f"firmware-{mode.removeprefix('build-')}"
            assert str(build_dir) in commands
            conf_arg = (
                f"-DEXTRA_CONF_FILE={source}/boards/"
                "esp32p4c5_core_board_esp32p4_hpcore.conf"
            )
            if mode == "build-p4-tls":
                conf_arg += (
                    f";{source}/boards/esp32p4c5_core_board_esp32p4_hpcore-tls.conf"
                )
            assert not any(arg.startswith("-DDTC_OVERLAY_FILE=") for arg in commands)
        elif mode == "build-p4-display":
            source = root / "firmware/tests/board/p4display"
            assert str(source) in commands
            assert str(root / "build/firmware-p4-display") in commands
            assert str(root / "firmware/app") not in commands
            conf_arg = f"-DEXTRA_CONF_FILE={source}/prj.conf"
            assert not any(arg.startswith("-DDTC_OVERLAY_FILE=") for arg in commands)
        elif mode in ("build-p4-boot-trace", "build-p4-touch-id"):
            source = root / "firmware/tests/board/p4touchid"
            assert str(source) in commands
            assert str(root / "build" / f"firmware-{mode.removeprefix('build-')}") in commands
            assert str(root / "firmware/app") not in commands
            conf_arg = f"-DEXTRA_CONF_FILE={source}/prj.conf"
            if mode == "build-p4-boot-trace":
                conf_arg += f";{source}/boottrace.conf"
            assert not any(arg.startswith("-DDTC_OVERLAY_FILE=") for arg in commands)
        else:
            assert not any(arg.startswith("-DDTC_OVERLAY_FILE=") for arg in commands)
            assert str(root / "build/firmware-p4") in commands
        assert conf_arg in commands
        assert "native_sim/native/64" not in "\n".join(commands)
    else:
        assert "hal_espressif" not in "\n".join(commands)
        assert "native_sim/native/64" in "\n".join(commands)
    if mode not in ("build-p4-net", "build-p4-tls"):
        assert str(nanopb) not in "\n".join(commands)
    if mode != "build-p4-tls":
        assert all(module not in "\n".join(commands) for module in tls_modules)
        if mode not in ("build-p4-display", "build-p4-boot-trace", "build-p4-touch-id"):
            assert "-DKCONFIG_WARNING_AS_ERROR=ON" not in commands
    if mode in ("build-p4-tls", "build-p4-display", "build-p4-boot-trace", "build-p4-touch-id"):
        assert "-DKCONFIG_WARNING_AS_ERROR=ON" in commands


@pytest.mark.parametrize(
    ("source", "content"),
    [
        ("subsys/task_wdt/task_wdt.c", "old\n"),
        ("drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c", "oldscan\n"),
        ("soc/espressif/common/loader.c", "oldloader\n"),
        ("drivers/input/input_cst8xx.c", "oldtouch\n"),
        ("drivers/display/display_esp32_dsi.c", "olddsi\n"),
        ("drivers/gpio/gpio_esp32.c", "oldgpio\n"),
    ],
)
def test_unpatched_zephyr_is_rejected_before_build(firmware_workspace, source, content):
    (firmware_workspace / ".deps/zephyr" / source).write_text(content)
    result = run_firmware(firmware_workspace, "build")
    assert result.returncode == 1
    assert "Missing or modified approved Zephyr patches" in result.stderr
    assert not (firmware_workspace / "build").exists()


@pytest.fixture
def setup_workspace(tmp_path):
    (tmp_path / "scripts").mkdir()
    (tmp_path / "patches").mkdir()
    (tmp_path / "patches/zephyr-task-wdt-setup.patch").write_text(TEST_WDT_PATCH)
    (tmp_path / "patches/zephyr-c5-scan.patch").write_text(TEST_SCAN_PATCH)
    (tmp_path / "patches/zephyr-p4-ram-digest.patch").write_text(TEST_DIGEST_PATCH)
    (tmp_path / "patches/zephyr-cst8xx-report.patch").write_text(TEST_TOUCH_PATCH)
    (tmp_path / "patches/zephyr-p4-dsi-frame-size.patch").write_text(TEST_DISPLAY_PATCH)
    (tmp_path / "patches/zephyr-esp32-gpio-base.patch").write_text(TEST_GPIO_PATCH)
    script = (ENV_DIR / "setup-firmware.sh").read_text()
    pins = re.findall(r'^\s+([0-9a-f]{40}) "\$ROOT/\.deps/([^"]+)"', script, re.M)
    for pin, name in pins:
        dep = tmp_path / ".deps" / name
        dep.mkdir(parents=True)
        subprocess.run(["git", "init", "--quiet", str(dep)], check=True)
        (dep / "CMakeLists.txt").touch()
        if name == "zephyr":
            gpio = dep / "drivers/gpio/gpio_esp32.c"
            gpio.parent.mkdir(parents=True)
            gpio.write_text("oldgpio\n")
            display = dep / "drivers/display/display_esp32_dsi.c"
            display.parent.mkdir(parents=True)
            display.write_text("olddsi\n")
            touch = dep / "drivers/input/input_cst8xx.c"
            touch.parent.mkdir(parents=True)
            touch.write_text("oldtouch\n")
            source = dep / "subsys/task_wdt/task_wdt.c"
            source.parent.mkdir(parents=True)
            source.write_text("old\n")
            scan = dep / "drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c"
            scan.parent.mkdir(parents=True)
            scan.write_text("oldscan\n")
            loader = dep / "soc/espressif/common/loader.c"
            loader.parent.mkdir(parents=True)
            loader.write_text("oldloader\n")
        if name == "hal_espressif":
            cpll = dep / "zephyr/esp32p4/src/soc_init.c"
            cpll.parent.mkdir(parents=True)
            cpll.write_text("oldcpll\n")
        subprocess.run(["git", "-C", str(dep), "add", "."], check=True)
        subprocess.run(
            ["git", "-C", str(dep), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
             "commit", "--quiet", "-m", "fixture"],
            check=True,
        )
        revision = subprocess.check_output(
            ["git", "-C", str(dep), "rev-parse", "HEAD"]
        ).decode().strip()
        script = script.replace(pin, revision)
    (tmp_path / "scripts/setup-firmware.sh").write_text(script)
    return tmp_path


def run_setup(workspace, mode="p4"):
    return subprocess.run(
        ["bash", str(workspace / "scripts/setup-firmware.sh"), mode],
        capture_output=True, text=True, timeout=10,
        env={"PATH": "/usr/bin:/bin", "HOME": str(workspace)},
    )


@pytest.mark.parametrize("mode", ["native", "p4", "p4-net", "p4-tls", "p4-sd"])
def test_approved_dependency_patch_is_repeatable(setup_workspace, mode):
    for _ in range(2):
        result = run_setup(setup_workspace, mode)
        assert result.returncode == 0, result.stderr
    assert (setup_workspace / ".deps/zephyr/subsys/task_wdt/task_wdt.c").read_text() == "new\n"
    assert (
        setup_workspace / ".deps/zephyr/drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c"
    ).read_text() == "newscan\n"
    assert (
        setup_workspace / ".deps/zephyr/soc/espressif/common/loader.c"
    ).read_text() == "newloader\n"
    assert (
        setup_workspace / ".deps/zephyr/drivers/input/input_cst8xx.c"
    ).read_text() == "newtouch\n"
    assert (
        setup_workspace / ".deps/zephyr/drivers/display/display_esp32_dsi.c"
    ).read_text() == "newdsi\n"
    assert (
        setup_workspace / ".deps/zephyr/drivers/gpio/gpio_esp32.c"
    ).read_text() == "newgpio\n"
    assert (
        setup_workspace / ".deps/hal_espressif/zephyr/esp32p4/src/soc_init.c"
    ).read_text() == "oldcpll\n"


@pytest.mark.parametrize("change", ["modified", "candidate", "untracked", "staged", "revision"])
def test_setup_rejects_unapproved_hal_changes(setup_workspace, change):
    assert run_setup(setup_workspace).returncode == 0
    hal = setup_workspace / ".deps/hal_espressif"
    source = hal / "zephyr/esp32p4/src/soc_init.c"
    if change == "modified":
        source.write_text("newcpll\nunapproved\n")
    elif change == "candidate":
        source.write_text("newcpll\n")
    elif change == "untracked":
        (hal / "unexpected.c").touch()
    elif change == "staged":
        source.write_text("staged change\n")
        subprocess.run(["git", "-C", str(hal), "add", "."], check=True)
    else:
        subprocess.run(
            ["git", "-C", str(hal), "-c", "user.name=Test",
             "-c", "user.email=test@example.invalid", "commit", "--quiet",
             "--allow-empty", "-m", "unexpected revision"],
            check=True,
        )
    result = run_setup(setup_workspace)
    assert result.returncode == 1
    assert "Dependency" in result.stderr
    if change == "modified":
        assert source.read_text() == "newcpll\nunapproved\n"


@pytest.mark.parametrize("mode", ["build-p4", "build-p4-boot-trace"])
def test_p4_build_does_not_require_or_apply_experimental_hal_patch(firmware_workspace, mode):
    root = firmware_workspace
    bin_dir = root / ".venv/bin"
    bin_dir.mkdir(parents=True)
    stub = bin_dir / "cmake"
    stub.write_text('#!/bin/bash\nprintf "%s\\n" "$@" >> "$HOME/commands"\n')
    stub.chmod(0o755)
    module = root / ".deps/hal_espressif/zephyr"
    module.mkdir(parents=True)
    (module / "module.yml").touch()
    cpll = module / "esp32p4/src/soc_init.c"
    cpll.parent.mkdir(parents=True)
    cpll.write_text("oldcpll\n")
    sdk = root / "sdk"
    sdk.mkdir()
    (sdk / "sdk_version").touch()
    result = run_firmware(root, mode, ZEPHYR_SDK_INSTALL_DIR=str(sdk))
    assert result.returncode == 0, result.stderr
    assert (root / "commands").exists()
    assert cpll.read_text() == "oldcpll\n"
    assert not (root / "patches/hal-esp32p4-cpll-config.patch").exists()


def test_setup_migrates_five_existing_patches(setup_workspace):
    zephyr = setup_workspace / ".deps/zephyr"
    (zephyr / "subsys/task_wdt/task_wdt.c").write_text("new\n")
    (zephyr / "drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c").write_text("newscan\n")
    (zephyr / "soc/espressif/common/loader.c").write_text("newloader\n")
    (zephyr / "drivers/input/input_cst8xx.c").write_text("newtouch\n")
    (zephyr / "drivers/display/display_esp32_dsi.c").write_text("newdsi\n")
    result = run_setup(setup_workspace)
    assert result.returncode == 0, result.stderr
    assert (zephyr / "drivers/gpio/gpio_esp32.c").read_text() == "newgpio\n"


def test_setup_migrates_four_existing_patches(setup_workspace):
    zephyr = setup_workspace / ".deps/zephyr"
    (zephyr / "subsys/task_wdt/task_wdt.c").write_text("new\n")
    (zephyr / "drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c").write_text("newscan\n")
    (zephyr / "soc/espressif/common/loader.c").write_text("newloader\n")
    (zephyr / "drivers/input/input_cst8xx.c").write_text("newtouch\n")
    result = run_setup(setup_workspace)
    assert result.returncode == 0, result.stderr
    assert (zephyr / "drivers/display/display_esp32_dsi.c").read_text() == "newdsi\n"


def test_setup_migrates_three_existing_patches(setup_workspace):
    zephyr = setup_workspace / ".deps/zephyr"
    (zephyr / "subsys/task_wdt/task_wdt.c").write_text("new\n")
    (zephyr / "drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c").write_text("newscan\n")
    (zephyr / "soc/espressif/common/loader.c").write_text("newloader\n")
    result = run_setup(setup_workspace)
    assert result.returncode == 0, result.stderr
    assert (zephyr / "drivers/input/input_cst8xx.c").read_text() == "newtouch\n"


def test_setup_migrates_watchdog_only_without_overwriting_it(setup_workspace):
    watchdog = setup_workspace / ".deps/zephyr/subsys/task_wdt/task_wdt.c"
    watchdog.write_text("new\n")
    result = run_setup(setup_workspace, "p4-net")
    assert result.returncode == 0, result.stderr
    assert watchdog.read_text() == "new\n"
    assert (
        setup_workspace / ".deps/zephyr/drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c"
    ).read_text() == "newscan\n"


def test_setup_migrates_two_existing_patches(setup_workspace):
    zephyr = setup_workspace / ".deps/zephyr"
    (zephyr / "subsys/task_wdt/task_wdt.c").write_text("new\n")
    (zephyr / "drivers/wifi/esp_hosted_mcu/esp_hosted_mcu.c").write_text("newscan\n")
    result = run_setup(setup_workspace, "p4-net")
    assert result.returncode == 0, result.stderr
    assert (zephyr / "soc/espressif/common/loader.c").read_text() == "newloader\n"


@pytest.mark.parametrize("mode", ["native", "p4", "p4-net"])
def test_setup_leaves_optional_dependencies_untouched(setup_workspace, mode):
    untouched = ["mbedtls", "tf-psa-crypto", "tf-psa-crypto-drivers", "fatfs"]
    if mode != "p4-net":
        untouched.append("nanopb")
    if mode == "native":
        untouched.append("hal_espressif")
    for name in untouched:
        (setup_workspace / ".deps" / name / "CMakeLists.txt").write_text("private edit\n")
    result = run_setup(setup_workspace, mode)
    assert result.returncode == 0, result.stderr
    for name in untouched:
        assert (
            setup_workspace / ".deps" / name / "CMakeLists.txt"
        ).read_text() == "private edit\n"


def test_network_setup_rejects_dirty_nanopb(setup_workspace):
    source = setup_workspace / ".deps/nanopb/CMakeLists.txt"
    source.write_text("private edit\n")
    result = run_setup(setup_workspace, "p4-net")
    assert result.returncode == 1
    assert "Dependency" in result.stderr
    assert source.read_text() == "private edit\n"


def test_sd_setup_rejects_dirty_fatfs(setup_workspace):
    source = setup_workspace / ".deps/fatfs/CMakeLists.txt"
    source.write_text("private edit\n")
    result = run_setup(setup_workspace, "p4-sd")
    assert result.returncode == 1
    assert "Dependency" in result.stderr
    assert source.read_text() == "private edit\n"


@pytest.mark.parametrize(
    "change", ["modified", "untracked", "staged", "revision", "other_dependency"]
)
def test_setup_rejects_unapproved_dependency_changes(setup_workspace, change):
    assert run_setup(setup_workspace).returncode == 0
    dep = setup_workspace / ".deps/zephyr"
    source = dep / "subsys/task_wdt/task_wdt.c"
    if change == "modified":
        source.write_text("new\nunapproved\n")
    elif change == "untracked":
        (dep / "unexpected.c").touch()
    elif change == "staged":
        subprocess.run(["git", "-C", str(dep), "add", "."], check=True)
    elif change == "revision":
        subprocess.run(
            ["git", "-C", str(dep), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
             "commit", "--quiet", "--allow-empty", "-m", "unexpected revision"],
            check=True,
        )
    else:
        (setup_workspace / ".deps/wamr/CMakeLists.txt").write_text("unapproved\n")
    result = run_setup(setup_workspace)
    assert result.returncode == 1
    assert "Dependency" in result.stderr
    if change == "modified":
        assert source.read_text() == "new\nunapproved\n", "Never overwrite unrelated edits"


@pytest.mark.parametrize(
    "name", ["hal_espressif", "nanopb", "mbedtls", "tf-psa-crypto", "tf-psa-crypto-drivers",
             "fatfs"]
)
def test_p4_dependency_pin_matches_pinned_zephyr_manifest(name):
    manifest = REPO / ".deps/zephyr/west.yml"
    if not manifest.is_file():
        pytest.skip("Pinned Zephyr source not installed")
    projects = yaml.safe_load(manifest.read_text())["manifest"]["projects"]
    revision = next(p["revision"] for p in projects if p["name"] == name)
    source = (ENV_DIR / "setup-firmware.sh").read_text()
    assert f'{revision} "$ROOT/.deps/{name}"' in source


def test_p4_bringup_has_explicit_small_app_storage_budget():
    config = (
        REPO / "firmware/app/boards/esp32p4c5_core_board_esp32p4_hpcore.conf"
    ).read_text()
    assert "CONFIG_TS_STORE_SLOT_SIZE=32768" in config
    assert "CONFIG_TS_APP_THREAD_STACK=16384" in config
    assert "CONFIG_TS_APP_WAMR=n" not in config
    assert "CONFIG_TS_SAFETY=n" not in config
    assert "CONFIG_TS_TEST=y" not in config
    assert "CONFIG_TS_WDT_REQUIRE_HW=y" in config
    assert "CONFIG_TASK_WDT_MIN_TIMEOUT=5000" in config
    assert "CONFIG_TASK_WDT_HW_FALLBACK_DELAY=5000" in config
    assert "CONFIG_TASK_WDT_HW_FALLBACK_PAUSE_HALTED_BY_DBG=n" in config


def test_p4_storage_profile_preserves_reserved_region_boundaries():
    board = REPO / "firmware/app/boards"
    prefix = "esp32p4c5_core_board_esp32p4_hpcore-store"
    overlay = (board / f"{prefix}.overlay").read_text()
    assert re.findall(r"/delete-node/\s+&(\w+);", overlay) == ["slot1_partition"]
    regions = [
        (int(start, 16), int(size, 16))
        for start, size in re.findall(r"reg = <(0x[0-9a-f]+) (0x[0-9a-f]+)>;", overlay)
    ]
    assert regions == [
        (0x7E0000, 0x1000), (0x7E1000, 0x2000),
        (0x7E3000, 0x8000), (0x7EB000, 0x8000),
        (0x7F3000, 0x1000), (0x7F4000, 0x2000), (0x7F6000, 0x7AA000),
    ]
    assert all(start % 4096 == size % 4096 == 0 for start, size in regions)
    assert all(start + size == next_start for (start, size), (next_start, _) in
               zip(regions, regions[1:], strict=False))
    assert regions[-1][0] + regions[-1][1] == 0xFA0000
    config = (board / f"{prefix}.conf").read_text()
    for option in ("FLASH", "FLASH_MAP", "TS_STORE_FLASH", "TS_STORE_AUDIT"):
        assert f"CONFIG_{option}=y" in config
    assert "CONFIG_TS_TEST=y" not in config


@pytest.mark.parametrize(
    "mode", ["build", "test", "build-p4", "build-p4-store", "build-p4-net", "build-p4-tls"]
)
def test_command_failure_stops_the_build(firmware_workspace, mode):
    root = firmware_workspace
    bin_dir = root / ".venv/bin"
    bin_dir.mkdir(parents=True)
    for command in ("cmake", "python"):
        stub = bin_dir / command
        stub.write_text(
            '#!/bin/bash\necho "tool failed" >&2\nexit 23\n',
            encoding="utf-8",
        )
        stub.chmod(0o755)
    module = root / ".deps/hal_espressif/zephyr"
    module.mkdir(parents=True)
    (module / "module.yml").touch()
    nanopb = root / ".deps/nanopb/zephyr"
    nanopb.mkdir(parents=True)
    (nanopb / "module.yml").touch()
    for name in ("mbedtls", "tf-psa-crypto", "tf-psa-crypto-drivers"):
        module = root / ".deps" / name / "zephyr"
        module.mkdir(parents=True)
        (module / "module.yml").touch()
    sdk = root / "sdk"
    sdk.mkdir()
    (sdk / "sdk_version").touch()
    result = run_firmware(root, mode, ZEPHYR_SDK_INSTALL_DIR=str(sdk))
    assert result.returncode == 23
    assert "tool failed" in result.stderr


def test_zenoh_pico_patch_diff_counts_selfconsistent():
    """统一 diff 逐 hunk 计数自检（计数/头行缺陷 = git apply 全拒——2026-10-24 实发并修复）。"""
    patch = (REPO / "firmware/module/tessera/ext/zenoh-pico-tls.patch").read_text(
        encoding="utf-8").splitlines()
    files: list[str] = []
    hunk = None
    old_n = new_n = 0
    # 文件段起点两种形态：diff --git 头（git 风格）或裸 --- a/ 头（本补丁首段 config.h 即此形态）

    def _close_hunk():
        assert hunk is not None and old_n == hunk[0] and new_n == hunk[1], (
            f"hunk 计数不实：头 @{hunk} 实际 old={old_n} new={new_n}")

    for ln in patch:
        if ln.startswith("diff --git "):
            if hunk:
                _close_hunk()
                hunk = None
            files.append(ln.split(" b/", 1)[0].split(" a/", 1)[1])
        elif ln.startswith("--- a/") and hunk is None and not files:
            files.append(ln[6:])  # 首段裸 --- 形态（config.h）——其余段 --- 跟随 diff --git 不重复计
        elif ln.startswith("@@ "):
            if hunk:
                _close_hunk()
            m = re.match(r"@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@", ln)
            assert m, f"hunk 头格式异常：{ln}"
            hunk = (int(m.group(1) or 1), int(m.group(2) or 1))
            old_n = new_n = 0
        elif hunk is not None:
            if ln.startswith("+++") or ln.startswith("---"):
                continue
            if ln.startswith("+"):
                new_n += 1
            elif ln.startswith("-"):
                old_n += 1
            elif ln.startswith(" ") or ln == "":
                old_n += 1
                new_n += 1
            else:
                raise AssertionError(f"补丁体异常行：{ln!r}")
    if hunk:
        _close_hunk()
    assert len(files) == len(set(files)) and len(files) >= 9, (
        f"TLS 补丁应 ≥9 文件（现行 {len(files)}）")
    # 每个文件段须有 ---/+++ 头（git apply 严格域——缺 --- 行 = 拒收）
    body = "\n".join(patch)
    for f in files:
        assert f"--- a/{f}\n" in body, f"{f} 段缺 --- a/ 头行"


def test_naming_convention_compliance():
    for p in ENV_DIR.rglob("*"):
        if p.is_file():
            stem = p.stem
            assert not re.fullmatch(r"[a-zA-Z]\d+(\.\d+)?", stem), f"命名违例：{p.name}"
    for d in ENV_DIR.iterdir():
        if d.is_dir():
            assert not re.fullmatch(r"[a-zA-Z]\d+", d.name), f"目录命名违例：{d.name}"
