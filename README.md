# Entelea

**An embodied AI framework: perceive, act, adapt, and manufacture new skills.**

Entelea combines a Zephyr device foundation with an independently versioned
engineering agent, [Entelea Forge](https://github.com/embalmer-Y/entelea-forge).
Optional affect kernels are extracted from Kaleido.

## Current baseline

- Migrated: Tessera firmware, hardware support, App sandbox/lifecycle, TSAP
  packaging contracts, safety checks, tests, and board utilities.
- Migrated: Forge's engineering pipeline, tests, skills, board profiles and web UI.
- Extracted: portable drive, emotion and ignition kernels with host regression.
- Not yet integrated: a general cognitive runtime, body-wide arbitration,
  Lua execution/hot-swap, and runtime affect-plugin lifecycle.

Existing `ts_*` APIs, Kconfig names and `tessera_agent` imports remain compatible.
Imported source is not represented as newly validated on every supported board.

## Start

```sh
git clone --recurse-submodules https://github.com/embalmer-Y/entelea.git
cd entelea
uv sync --locked
uv sync --project agent --locked --extra dev
make check
make agent-test
```

Install `uv`, a C compiler, CMake/Ninja, and LLVM clang with WebAssembly support.
Firmware setup and board constraints are in [development](docs/development.md).

## Layout

| Path | Purpose |
| --- | --- |
| `firmware/` | Zephyr framework, board definitions and regression/bench suites |
| `lib/affect/` | Optional portable affect kernels; not automatically linked into firmware |
| `agent/` | Pinned Forge Git submodule |
| `scripts/` | Small reproducible validation/setup commands |
| `patches/` | Explicit external dependency patches |
| `docs/` | Current architecture, safety, development and provenance |

Read [architecture](docs/architecture.md), [safety](docs/safety.md), and
[provenance](docs/provenance.md). Apache-2.0 applies unless a file or retained
third-party notice specifies otherwise. Galatea-derived affect code retains
its MIT attribution in [LICENSES](LICENSES/Galatea-MIT.txt).
