# Migration provenance

## Imported sources

- [Tessera](https://github.com/embalmer-Y/Tessera/tree/64a2cb7b2cb010be1951bf4a25e153f41519efad):
  `firmware/`, `tools/board/`, license and attributes. Its `agent/` subtree is
  independently imported into Forge. Existing notices remain in place.
- Kaleido commit `3fbc72266b25e4eee1a0667b7d7af4f9e7157ee9`:
  `kaleido-fw/components/kaleido_{drive,emotion,ignition}` and their golden tests,
  extracted under `lib/affect/`. The original repositories remain unchanged.
- Galatea-derived affect code retains its MIT license and source attribution.
- The imported TweetNaCl code retains its own provenance notice.

The `import-baseline` Git tag records imported source before Entelea adaptations.
Subsequent commits identify modifications. Historical DEC/Q/HLD/LLD references
in that source resolve against the Tessera commit above. Old plans, research
reports and session registers were deliberately not copied into the live docs.

## External dependencies

| Dependency | Pinned revision |
| --- | --- |
| Zephyr v4.5.0 | `41d10b59730406bbab4ca9f1fbbe9e96aafb4966` |
| WAMR 2.4.5 | `25bd7eb63e828e4bd242cc9b38d260b4b31c6605` |
| zenoh-pico 1.10.1 | `e1ab223a28aaebb5dec1e70d98eab152332f777a` |
| Unity 2.6.1, host tests only | `cbcd08fa7de711053a3deec6339ee89cad5d2697` |

`patches/zephyr-esp32-gdma.patch` preserves a previously uncommitted modification
in the existing Zephyr workspace. It is not silently treated as upstream.
Apply/review it only for the corresponding ESP32 DMA board validation; native_sim
does not exercise it. The existing zenoh TLS patch remains in
`firmware/module/tessera/ext/zenoh-pico-tls.patch`.

Public test signing keys and packages are development fixtures, not credentials
for deployment. Never provision a production device with those roots.

## Deliberately excluded

Local credentials/configuration, runtime workspaces, build caches, session logs,
full decision histories, unfinished Lua implementation claims, and the ESP-IDF
conversation/UI shell. Original repositories remain the historical source.

The old `tools/env` installer was replaced with isolated source setup. Its
installer-specific tests were updated for the replacement, retaining TLS patch
integrity coverage. Forge gateway subprocess tests use a temporary workspace
rather than assuming the parent directory contains firmware.
