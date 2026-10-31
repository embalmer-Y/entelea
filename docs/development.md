# Development

## Host baseline

Linux x86-64 is the initial verified host; native_sim/native/64 also avoids an
unnecessary 32-bit multilib dependency. Use Python 3.12, `uv`, GCC, CMake, Ninja,
and clang with wasm32 support. `uv.lock` pins host Python tools; Forge has its
own independent lockfile.

```sh
uv sync --locked
uv sync --project agent --locked --extra dev
make check
make agent-test
```

The affect CMake build fetches pinned Unity into `build/`, or accepts
`-DFETCHCONTENT_SOURCE_DIR_UNITY=/absolute/path/to/Unity` for offline use.
No ESP-IDF installation is required.

## Firmware

To prepare a clean, isolated native_sim source baseline:

```sh
bash scripts/setup-firmware.sh
make firmware-build
make firmware-test
```

Sources live in ignored `.deps/`. These commands do not update another
project's workspace, install system packages, change shell startup files,
flash devices or contact an LLM.

Existing source checkouts can be used read-only:

```sh
export ZEPHYR_BASE=/absolute/path/to/zephyr
export TS_WAMR_DIR=/absolute/path/to/wamr
make firmware-test
```

Use the clean isolated baseline for reproducibility. A modified external
checkout is not equivalent to the pinned revision.

Board work additionally requires the Zephyr SDK, the board's west modules,
vendor blobs where applicable, network configuration and explicit hardware
approval. Start from Zephyr's pinned west manifest and the retained board
definitions. The ESP32 DMA and zenoh TLS patches are tracked explicitly; host
tests do not validate their physical behavior. No automatic board flashing is
part of setup or CI.

## Checks and performance

- `make check`: source hygiene, setup/hygiene regressions, safety invariants,
  affect CTest.
- `make firmware-build`: host production firmware build.
- `make firmware-test`: native_sim Twister regression.
- `make agent-test`: Forge tests and fixture equivalence against this checkout.
- `make style BASE=<commit>`: changed C/H clang-format and the pinned Zephyr
  kernel-derived checkpatch, excluding upstream crypto. Run firmware setup first.
  Unity-required API names have a narrow CamelCase exception in its test runner.
  Imported style debt is not fixed en masse during behavioral migration.

For C review, follow the
[Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html).
Formatting is only part of this: locking, allocation, error paths and data
layout still need review. Require measured evidence for performance changes,
with target, configuration, workload and before/after values. Do not trade
away safety/correctness to improve a benchmark.

The initial local baseline passes 17 native_sim configurations (115 cases),
36 affect cases, 17 setup/hygiene tests and 7 safety checks. Forge passes
396 tests with the firmware checkout, or 393 standalone; deployment and Zephyr
E2E simulation remain opt-in, and standalone fixture comparisons are skipped.
The inherited FastMCP schema deprecation warning remains to be addressed.

## Forge

`agent/` is a real Git submodule. Commit and push Forge first, then update its
pointer in Entelea. `uv run --project agent entelea-forge --help` is the branded
entry point; the old command/import/environment names remain compatible.
Copy `agent/config.example.toml` to ignored `agent/config.toml` and explicitly
set the firmware workspace/model. API keys stay in the process environment.
No default model credentials are copied from existing projects.
Forge assets default to its own source checkout (`agent.forge_repo`), not the
firmware checkout's parent/child relationship. Writable factory workspaces keep
the inherited firmware-root layout during this migration.

## Merge policy

Protect the main branches with required CI checks. Changes to CI, instructions,
public contracts and safety/trust code require owner review. CODEOWNERS records
ownership, but enforcement requires repository branch rules; a file alone
cannot prevent the agent from changing it.
