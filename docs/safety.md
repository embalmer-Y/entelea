# Device safety invariants

These preserve the Tessera migration baseline. A change requires owner approval
and corresponding behavioral tests; this document is not a safety certification.

1. Every output, including controlled power, declares power-on, link-loss and
   fault states. Undeclared channels cannot be registered.
2. Hardware output goes through permission checks and the safety commit path
   before driver dispatch, including amplitude, slew and power limits.
3. Orchestrated devices enter the declared link-loss state on host loss.
   Standalone devices record link loss without transferring local control.
   Provisioning selects the mode. Safety does not suppress available input data.
4. Watchdogs and fault handling remain independent of AI inference.
5. Emergency stop uses its dedicated hardware path, not a model, transport or
   queued App callback. Clearing it must not itself trigger motion.
6. Initialization follows the established order. System failures cannot leave
   uncontrolled partially initialized outputs; App failures use their lifecycle
   isolation and rollback policy.
7. Controlled power is an output and follows the same protections.
8. Local protections do not depend on another device being reachable.
9. Deterministic control uses the injected monotonic clock and replayable inputs,
   not unseeded randomness or unordered iteration.
10. AI, Apps and affect cannot rewrite safety limits, permissions or trust roots.
11. Local/remote source arbitration is preserved. Future multi-App channel
    arbitration must strengthen, not bypass, these checks.

A successful command receipt is not proof of physical completion. Board timing,
stop behavior and safe values require measurements on the actual mechanism.
