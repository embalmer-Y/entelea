#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Rebuild this repository's pinned development environment (owner decision
# 2026-11-23, rebuild-plan section 10.6). Every piece is verified with the
# repository's own read-only checker; nothing is patched, installed or
# accepted silently and there is no fallback to an unpinned source.
#
# Layout (build-support/README.md, "工作区、Zephyr 构建与 SDK 目录合同"):
#   .venv/                    build interpreter, CPython 3.12, never west
#   .deps/zephyrproject/      west workspace: Zephyr at the pinned commit
#                             with the approved patches, plus pinned modules
#   .deps/{wamr,lua,esp-hosted-mcu,zephyr-sdk-<version>}
#   <env-root>/west           west itself, pinned
#
# Usage: build-support/setup-env.sh [options]
#   --system        install the host packages with apt (needs sudo)
#   --proxy URL     prefix for GitHub URLs where direct access is slow,
#                   for example https://ghfast.top/ (sources are verified)
#   --env-root DIR  directory of the west environment
#                   (default: <parent of this repository>/envs)
#   --skip-sdk      skip the Zephyr SDK (only board builds need it)
#   --skip-modules  skip the west-managed Zephyr modules
#   -h, --help      show this text
#
# Re-running is safe: a stage whose verified state already exists is skipped.
set -euo pipefail

usage() {
	sed -n '2,/^set -euo pipefail/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'
}

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/.deps"
LOCK="$ROOT/build-support/requirements-zephyr.lock"
PINS="$ROOT/build-support/dependencies.json"
CHECK="$ROOT/tools/check_dependencies.py"
WORKSPACE="$DEPS/zephyrproject"
ZEPHYR_BASE="$WORKSPACE/zephyr"
WEST_VERSION=1.5.0 # the version the original environment used (RF1 record)
SDK_VERSION=1.0.1
SDK="$DEPS/zephyr-sdk-$SDK_VERSION"
SDK_BASE="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v$SDK_VERSION"
SDK_BUNDLE="zephyr-sdk-${SDK_VERSION}_linux-x86_64_minimal.tar.xz"
SDK_TOOLCHAIN="toolchain_gnu_linux-x86_64_riscv64-zephyr-elf.tar.xz"
ZEPHYR_URL="https://github.com/zephyrproject-rtos/zephyr.git"
# Upstream locations of the directly checked-out sources; the commits come
# from dependencies.json, which check_dependencies.py cross-checks.
SOURCES=(
	"wamr https://github.com/bytecodealliance/wasm-micro-runtime.git wamr"
	"lua https://github.com/lua/lua.git lua"
	"esp_hosted_mcu https://github.com/espressif/esp-hosted-mcu.git esp-hosted-mcu"
)
# west project names of the pinned modules (west.yml spells tf_psa_crypto
# "tf-psa-crypto").
WEST_MODULES=(hal_espressif nanopb mbedtls tf-psa-crypto)
# Debian/Ubuntu packages of the host toolchain. uv is not packaged by
# apt: it is a prerequisite (https://docs.astral.sh/uv/).
HOST_PACKAGES=(
	cmake ninja-build gcc-multilib gperf device-tree-compiler git curl xz-utils
	tar perl
)
SYSTEM=false
PROXY=""
ENV_ROOT="$(dirname "$ROOT")/envs"
SDK_WANTED=true
MODULES_WANTED=true

while [ "$#" -gt 0 ]; do
	case "$1" in
	--system) SYSTEM=true; shift ;;
	--proxy) PROXY="${2:?--proxy needs a URL}"; shift 2 ;;
	--proxy=*) PROXY="${1#--proxy=}"; shift ;;
	--env-root) ENV_ROOT="${2:?--env-root needs a directory}"; shift 2 ;;
	--skip-sdk) SDK_WANTED=false; shift ;;
	--skip-modules) MODULES_WANTED=false; shift ;;
	-h | --help) usage; exit 0 ;;
	*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
	esac
done
case "$PROXY" in
"") ;;
https://*) PROXY="${PROXY%/}/" ;;
*) echo "--proxy must be an https:// URL prefix" >&2; exit 2 ;;
esac
WEST_ENV="$ENV_ROOT/west"
GH="${PROXY}https://github.com"

say() { echo "== $*"; }
die() { echo "FAIL: $*" >&2; exit 1; }

# Git options that keep large fetches from stalling on a slow link.
GIT_STURDY=(-c http.version=HTTP/1.1 -c http.lowSpeedLimit=1 -c http.lowSpeedTime=120)

# dep <id> <field>: one value from build-support/dependencies.json, the
# single source of the pins (lists come back space-separated; a patch list as
# its paths). A missing field aborts: the pins are never guessed.
dep() {
	local out
	out="$(python3 - "$PINS" "$1" "$2" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
item = next(d for d in data["dependencies"] if d["id"] == sys.argv[2])
value = item[sys.argv[3]]
if isinstance(value, list):
    value = " ".join(v["path"] if isinstance(v, dict) else v for v in value)
print(value)
PY
	)" || die "dependencies.json has no $2 for $1"
	printf '%s\n' "$out"
}

check() { # check <dependency-id> <checkout>: the repository's own verifier
	python3 "$CHECK" --dependency "$1" --checkout "$2" ||
		die "$1 is not the pinned, clean checkout"
}

# Pinned shallow checkout of one commit. With patches, the complete working
# tree diff must equal their concatenation, the rule the checker enforces.
fetch_pinned() { # fetch_pinned <upstream-url> <rev> <dest> [patch...]
	local url="$1" rev="$2" dest="$3"
	shift 3
	local patches=("$@") missing=() patch actual
	if [ ! -e "$dest" ]; then
		mkdir -p "$(dirname "$dest")"
		git init --quiet "$dest"
	fi
	if git -C "$dest" remote get-url origin >/dev/null 2>&1; then
		# A retry may switch between the direct route and the proxy.
		git -C "$dest" remote set-url origin "$GH/${url#https://github.com/}"
	else
		git -C "$dest" remote add origin "$GH/${url#https://github.com/}"
	fi
	if ! git -C "$dest" rev-parse --verify HEAD >/dev/null 2>&1; then
		git -C "$dest" "${GIT_STURDY[@]}" fetch --depth 1 origin "$rev"
		git -C "$dest" checkout --quiet --detach "$rev"
	fi
	[ "$(git -C "$dest" rev-parse HEAD)" = "$rev" ] ||
		die "$dest is at $(git -C "$dest" rev-parse --short HEAD), expected $rev"
	[ -z "$(git -C "$dest" diff --cached --name-only)" ] &&
		[ -z "$(git -C "$dest" ls-files --others --exclude-standard)" ] ||
		die "$dest has staged or untracked files"
	[ "${#patches[@]}" -gt 0 ] || return 0
	for patch in "${patches[@]}"; do
		if git -C "$dest" apply --reverse --check "$patch" >/dev/null 2>&1; then
			continue # already applied
		fi
		git -C "$dest" apply --check "$patch" 2>/dev/null ||
			die "$dest: $(basename "$patch") does not apply"
		missing+=("$patch")
	done
	[ "${#missing[@]}" -eq 0 ] || git -C "$dest" apply "${missing[@]}"
	actual="$(git -C "$dest" diff --no-ext-diff --no-color --no-textconv \
		--unified=3 --binary HEAD | sed '/^index /d')"
	[ "$actual" = "$(cat "${patches[@]}")" ] ||
		die "$dest differs from the approved patch set"
}

stage_system() {
	$SYSTEM || { say "host packages: not requested (--system installs them)"; return; }
	local missing=() package
	for package in "${HOST_PACKAGES[@]}"; do
		dpkg -s "$package" >/dev/null 2>&1 || missing+=("$package")
	done
	if [ "${#missing[@]}" -eq 0 ]; then
		say "host packages: already installed"
	else
		sudo apt-get install -y --no-install-recommends "${missing[@]}"
	fi
}

preflight() {
	local tool probe
	for tool in git curl xz tar perl python3 gcc; do
		command -v "$tool" >/dev/null ||
			die "$tool is missing (run with --system)"
	done
	command -v uv >/dev/null ||
		die "uv is missing (https://docs.astral.sh/uv/)"
	# native_sim is a 32-bit target: the host compiler must build it.
	probe="$(mktemp -d)"
	if ! printf 'int main(void){return 0;}\n' |
		gcc -m32 -x c - -o "$probe/probe" 2>/dev/null; then
		die "the host compiler cannot build 32-bit binaries; native_sim needs gcc-multilib"
	fi
	rm -rf "$probe"
}

stage_venv() {
	say "build interpreter .venv (CPython 3.12, hashed lock, no west)"
	[ -x "$ROOT/.venv/bin/python3" ] || uv venv --seed --python 3.12 "$ROOT/.venv"
	uv pip sync --require-hashes --python "$ROOT/.venv/bin/python3" "$LOCK"
	"$ROOT/.venv/bin/python3" -c "import importlib.util,sys; sys.exit(3 if importlib.util.find_spec('west') else 0)" ||
		die "west is importable from .venv; Zephyr would inherit an unverified module set"
}

stage_west() {
	say "west $WEST_VERSION in its own environment"
	[ -x "$WEST_ENV/bin/west" ] || uv venv --python 3.12 "$WEST_ENV"
	[ "$("$WEST_ENV/bin/west" --version)" = "West version: v$WEST_VERSION" ] ||
		uv pip install --python "$WEST_ENV/bin/python3" "west==$WEST_VERSION"
	"$WEST_ENV/bin/west" --version
}

stage_zephyr() {
	local patches=() paths=() rev path
	rev="$(dep zephyr commit)"
	read -r -a paths <<<"$(dep zephyr patches)"
	say "Zephyr ${rev:0:7} with its ${#paths[@]} approved patches"
	for path in "${paths[@]}"; do
		patches+=("$ROOT/$path")
	done
	[ "${#patches[@]}" -gt 0 ] || die "dependencies.json lists no Zephyr patches"
	fetch_pinned "$ZEPHYR_URL" "$rev" "$ZEPHYR_BASE" "${patches[@]}"
	check zephyr "$ZEPHYR_BASE"
}

stage_modules() {
	$MODULES_WANTED || { say "Zephyr modules: skipped"; return; }
	local name id path
	say "pinned Zephyr modules via west (${WEST_MODULES[*]})"
	[ -d "$WORKSPACE/.west" ] || ( cd "$WORKSPACE" && "$WEST_ENV/bin/west" init -l zephyr )
	# Route GitHub through the proxy for this command only; the user's git
	# configuration is not modified.
	(
		if [ -n "$PROXY" ]; then
			export GIT_CONFIG_COUNT=1
			export GIT_CONFIG_KEY_0="url.${PROXY}https://github.com/.insteadOf"
			export GIT_CONFIG_VALUE_0=https://github.com/
		fi
		cd "$WORKSPACE" && "$WEST_ENV/bin/west" update "${WEST_MODULES[@]}"
	)
	for name in "${WEST_MODULES[@]}"; do
		id="${name//-/_}"
		path="$( cd "$WORKSPACE" && "$WEST_ENV/bin/west" list --format='{path}' "$name" )"
		check "$id" "$WORKSPACE/$path"
	done
}

stage_sources() {
	local entry id url dir rev
	say "WAMR, Lua and esp-hosted-mcu at their pinned commits"
	for entry in "${SOURCES[@]}"; do
		read -r id url dir <<<"$entry"
		rev="$(dep "$id" commit)"
		fetch_pinned "$url" "$rev" "$DEPS/$dir"
		check "$id" "$DEPS/$dir"
	done
}

# Download one official SDK archive and verify it against the release's
# sha256.sum; an interrupted download resumes on the next run.
sdk_fetch() { # sdk_fetch <archive>: prints the verified local path
	local archive="$1" sum="$DEPS/.sdk-$SDK_VERSION.sha256" want have
	[ -s "$sum" ] || curl -sSL --retry 5 --connect-timeout 20 --max-time 600 \
		-o "$sum" "$SDK_BASE/sha256.sum"
	grep -q " ${SDK_BUNDLE}\$" "$sum" || die "could not fetch the official sha256.sum"
	want="$(grep -E "[[:space:]\*]${archive//./\\.}\$" "$sum" | awk '{print $1}')"
	[ -n "$want" ] || die "no checksum for $archive"
	have="$(sha256sum "$DEPS/$archive" 2>/dev/null | awk '{print $1}')"
	if [ "$have" != "$want" ]; then
		curl -sSL --connect-timeout 20 --max-time 1800 -C - \
			-o "$DEPS/$archive" "$SDK_BASE/$archive" || true
		have="$(sha256sum "$DEPS/$archive" | awk '{print $1}')"
	fi
	[ "$have" = "$want" ] ||
		die "$archive is incomplete (have $have); re-run to resume"
	echo "$DEPS/$archive"
}

stage_sdk() {
	local bundle toolchain stage
	$SDK_WANTED || { say "Zephyr SDK: skipped"; return; }
	if [ -f "$SDK/sdk_version" ] && [ -d "$SDK/gnu/riscv64-zephyr-elf" ] &&
		[ -d "$SDK/hosttools/sysroots" ]; then
		say "Zephyr SDK $SDK_VERSION: already assembled"
	else
		say "Zephyr SDK $SDK_VERSION (official archives, sha256-verified)"
		bundle="$(sdk_fetch "$SDK_BUNDLE")"
		toolchain="$(sdk_fetch "$SDK_TOOLCHAIN")"
		if [ ! -f "$SDK/sdk_version" ]; then
			stage="$(mktemp -d "$DEPS/.sdk-stage.XXXXXX")"
			tar -xJf "$bundle" -C "$stage"
			[ -d "$stage/zephyr-sdk-$SDK_VERSION" ] ||
				die "unexpected bundle layout"
			mv "$stage/zephyr-sdk-$SDK_VERSION" "$SDK"
			rmdir "$stage"
		fi
		if [ ! -d "$SDK/gnu/riscv64-zephyr-elf" ]; then
			mkdir -p "$SDK/gnu"
			tar -xJf "$toolchain" -C "$SDK/gnu"
		fi
		# The bundle ships the host-tools installer. Run it the way
		# setup.sh -h does; the CMake package registration (setup.sh -c) is
		# deliberately not done, so nothing is written outside .deps.
		if [ ! -d "$SDK/hosttools/sysroots" ]; then
			( cd "$SDK/hosttools" &&
				./zephyr-sdk-x86_64-hosttools-standalone-0.10.sh -y -d . >/dev/null )
		fi
	fi
	[ "$(cat "$SDK/sdk_version")" = "$SDK_VERSION" ] || die "unexpected sdk_version"
	"$SDK/gnu/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-gcc" --version | head -n 1
}

say "environment setup for $ROOT"
preflight
stage_system
stage_venv
stage_west
stage_zephyr
stage_modules
stage_sources
stage_sdk
say "done. Put .venv first on PATH for Zephyr builds (nanopb's generator"
say "needs the grpcio-tools installed there):"
say "  PATH=\"$ROOT/.venv/bin:\$PATH\" python3 tools/build_zephyr.py --help"
