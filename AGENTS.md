# Entelea development contract

Entelea is a Zephyr-based embodied AI framework. Forge, in `agent/`, is an
independently versioned engineering agent, not the device's runtime brain.

## Scope and authority

- Implement the requested outcome without unrelated cleanup or speculative APIs.
- Preserve the safety invariants in [docs/safety.md](docs/safety.md).
- The owner controls product direction. Internal implementation choices are
  delegated. Obtain approval for safety/trust changes, incompatible public
  contracts, new dependencies, destructive migration, and hardware deployment.
- Never modify checks merely to make a failing implementation pass.
- Do not flash hardware, change provisioning, or start paid model calls as part
  of ordinary tests.

## Code

- C follows Linux kernel coding style: tabs of width 8, Linux braces, short
  functions, descriptive names, and 100-column guidance.
- Performance precedes readability, subject to correctness and hardware safety.
  Measure latency, throughput, CPU and memory before accepting an optimization.
  Document the measured trade-off rather than claiming that code "looks faster".
- Python follows the existing Ruff configuration; do not apply C formatting to it.
- Keep `ts_*`, `CONFIG_TS_*`, TSAP and the `tessera_agent` import namespace
  compatible during migration. Product renaming does not justify ABI churn.
- Reuse existing mechanisms. Do not add unused plugin layers or fallback paths.
- Keep errors explicit. Add regression tests for changed behavior.

## Validation

- Setup and commands: [docs/development.md](docs/development.md).
- `make check` runs repository checks, safety checks and host affect tests.
- `make firmware-test` runs native_sim firmware tests.
- `make agent-test` runs Forge tests with cross-repository fixture checks.
- `make style BASE=<commit>` checks changed C/H files. `import-baseline` is
  the initial style-debt boundary, not a claim that imported code is style-clean.
- Report exact checks and failures. Host tests do not establish board timing.

## Repository hygiene

- Root instructions must remain under 8 KiB and 100 lines.
- Do not put session histories, completed plans or recurring status reports here.
- Update the existing authoritative document when a public behavior changes.
  Create a persistent document only for a distinct maintainer/user need.
- Temporary plans and reports belong outside tracked source.
- Historical DEC/Q references in imported code resolve through
  [docs/provenance.md](docs/provenance.md), not copied decision registers.
- Do not commit credentials, local configuration, caches or generated builds.
- Third-party and generated code retain their upstream format and notices.
