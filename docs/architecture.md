# Architecture and migration boundary

The system has three separate responsibilities:

1. **Body:** Zephyr, hardware drivers, safety, permissions, App execution,
   sensing, and eventually bounded reflexes and shared-channel arbitration.
2. **Cognition:** future task/intent/feedback orchestration with optional affect.
   It must not block the body or acquire authority to change safety limits.
3. **Factory:** Forge manufactures and validates capabilities. Its absence must
   not prevent already-installed local behavior from running.

## What exists

The Tessera firmware is retained under `firmware/module/tessera`. Its namespace
is a compatibility boundary, not the new product name. WAMR is the implemented
App engine; Lua and engine-independent hot-swap remain design work.

`lib/affect` contains Kaleido's single-writer, singleton C kernels and their
original golden tests. They build without ESP-IDF. They are not yet a general
multi-instance plugin API and are not wired into the Zephyr execution path.
The fixed social/stimulation/rest model is a companion policy, not a mandatory
dependency of task-oriented devices.

Forge is a Git submodule at `agent/` so existing firmware-domain paths remain
compatible. Forge also runs independently; cross-repository checks explicitly
use `ENTELEA_ROOT` rather than assuming a parent directory layout.

## Next integration order

Validate the target board's concurrent media/control budget, then implement
typed skill receipts and channel ownership, bounded Lua lifecycle, cognitive
orchestration, and optional affect lifecycle. Do not combine those behavior
changes with source relocation or cosmetic namespace replacement.

Historical design proposals are referenced by upstream commit in provenance.
They are not current implementation claims or automatically approved requirements.
