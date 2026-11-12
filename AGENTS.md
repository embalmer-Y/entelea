# Entelea agent entry

This tree contains the R00-R12 implementation milestones plus the RF1 review
repairs. Historical milestones and diagnostic demonstrations are not complete
framework or product acceptance. RF1 still has open items (D12 guard, native trust boundary, R10 persistence,
Lua feasibility, Forge anchor, final review, factory permission mapping); R13
product acceptance remains pending. See `docs/rf1-validation.md`.

- Read README and the current batch in `docs/rebuild-plan.md` first.
- Use `docs/migration-assets.json` as the sole migration/status ledger. Query only
  relevant asset IDs; do not inject the entire generated inventory into context.
- D01-D12 have owner-approved directions, not fully frozen details. Read section 7
  of the plan before implementing unresolved boundaries. D03 requires first-phase
  Lua contracts and feasibility validation, not first-phase production support.
- Forge uses owner-approved OpenHands Software Agent SDK, not the legacy generic
  creation loop. The factory is merged into `factory/forge/` (owner decision,
  2026-11-12); its SDK environment is `/home/emb/project/envs/openhands`, synced
  from a hashed lock. Isolation, egress and product acceptance remain to be verified.
- Distinguish framework contracts from product requirements. Use auxiliary
  intelligence / simulated intelligence at framework level; companion only in
  the companion product context.
- The sibling `entelea-legacy` is frozen. Never modify its source, delete its
  caches, or remove its Git object store: this tree is a linked worktree.
- Do not recursively ingest legacy code, dependency trees, environments, build
  caches or logs; query fixed source objects and asset IDs as needed.
- Read legacy assets from the pinned commits in the ledger. Inventory coverage
  is not a claim that every source line was audited.
- Work only within an explicitly authorized batch. Record exact source selections and target paths,
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
  explicit authorization. The old Forge baseline is pinned as legacy gitlink `agent`
  (ca5219e7); the in-repo factory snapshot is ec24f00 (history: bundle).
- Zephyr builds use an explicit module set. Never install west into the build
  interpreter (`.venv`); `tools/build_zephyr.py` refuses such an interpreter.
- Keep migration state separate from verification. Record failures and untested
  boundaries; never weaken assertions or exclusions to manufacture green tests.
- Common primitive contracts are in `framework/common/contract.md`; build sources
  are explicitly classified in `build-support/source-policy.json`. Diagnostics
  must be separate exact targets, never common/production source exemptions.
- Before marking a batch done, run its applicable behavior tests, the registered
  CMake/CTest integration, `python3 tools/check_migration.py --sources`, and
  `python3 -m unittest discover -s tools/tests -v`. Report unavailable test
  integrations as blocked rather than claiming they passed.
- The metadata checks do not validate firmware behavior. Legacy L5/Python checks
  have a documented failure; read the plan before changing their scope.
- Commit implementation before recording its commit in the ledger, then commit
  the ledger update. Retain third-party attribution and license files.
- Keep instructions short; one local contract per implemented module as needed.
  Do not copy old design discussions into competing architecture/progress files.
