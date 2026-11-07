# Entelea agent entry

This tree contains a rebuild governance skeleton, not an implemented framework.

- Read README and the current batch in `docs/rebuild-plan.md` first.
- Use `docs/migration-assets.json` as the sole migration/status ledger. Query only
  relevant asset IDs; do not inject the entire generated inventory into context.
- Confirm required design decisions before implementation. D01-D12 are proposals
  awaiting owner review, not implicitly approved by permission to archive.
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
- Before marking a batch done, run the smallest applicable behavior tests and:
  `python3 tools/check_migration.py --sources`
  `python3 -m unittest discover -s tools/tests -v`
- The metadata checks do not validate firmware behavior. Legacy L5/Python checks
  have a documented failure; read the plan before changing their scope.
- Commit implementation before recording its commit in the ledger, then commit
  the ledger update. Retain third-party attribution and license files.
- Keep instructions short; one local contract per implemented module as needed.
  Do not copy old design discussions into competing architecture/progress files.
