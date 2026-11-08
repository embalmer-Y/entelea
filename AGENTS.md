# Entelea agent entry

This tree has R01 common primitives, the R02 supervision/output-protection
software loop (board bring-up and the hardware gate blocked on D12), and the
R03 storage batch in progress (port/meta/noinit landed; audit/prov/trust/KV
pending). It is not an implemented device runtime or product.

- Read README and the current batch in `docs/rebuild-plan.md` first.
- Use `docs/migration-assets.json` as the sole migration/status ledger. Query only
  relevant asset IDs; do not inject the entire generated inventory into context.
- D01-D12 have owner-approved directions, not fully frozen details. Read section 7
  of the plan before implementing unresolved boundaries. D03 requires first-phase
  Lua contracts and feasibility validation, not first-phase production support.
- Forge uses owner-approved OpenHands Software Agent SDK, not the legacy generic
  creation loop. Section 7.1 of the plan fixes the engine choice; version,
  isolation and integration remain to be verified in an approved factory scope.
- Distinguish framework contracts from product requirements. Use auxiliary
  intelligence / simulated intelligence at framework level; companion only in
  the companion product context.
- The sibling `entelea-legacy` is frozen. Never modify its source, delete its
  caches, or remove its Git object store: this tree is a linked worktree.
- Read legacy assets from the pinned commits in the ledger. Inventory coverage
  is not a claim that every source line was audited.
- Work on one ready batch. Record exact source selections and target paths,
  preserve applicable behavior, and migrate needed tests with each mechanism.
- Do not copy whole old modules, create empty future directories, silently
  expand scope, rename public ABI/protocols, or turn diagnostic code into
  production behavior without an explicit reviewed contract.
- Never bypass safety, authorization, signatures, user acceptance, or egress
  policy, including development configurations.
- Surface errors and unknown outcomes; acceptance is not completion, cancellation
  cannot undo effects, and unknown non-idempotent operations must not be retried.
- Preserve other agents' changes; pause on conflicts. Do not reset or amend their
  work. No pushes, real cloud/paid calls, flashing, or hardware actions without
  explicit authorization. Forge is an independent pinned repository.
- Keep migration state separate from verification. Record failures and untested
  boundaries; never weaken assertions or exclusions to manufacture green tests.
- Common primitive contracts are in `framework/common/contract.md`; build sources
  are explicitly classified in `build-support/source-policy.json`. Diagnostics
  must be separate exact targets, never common/production source exemptions.
- Before marking a batch done, run the smallest applicable behavior tests and:
  `cmake -S . -B build/r01 && cmake --build build/r01`
  `ctest --test-dir build/r01 --output-on-failure`
  `python3 tools/check_migration.py --sources`
  `python3 -m unittest discover -s tools/tests -v`
- The metadata checks do not validate firmware behavior. Legacy L5/Python checks
  have a documented failure; read the plan before changing their scope.
- Commit implementation before recording its commit in the ledger, then commit
  the ledger update. Retain third-party attribution and license files.
- Keep instructions short; one local contract per implemented module as needed.
  Do not copy old design discussions into competing architecture/progress files.
