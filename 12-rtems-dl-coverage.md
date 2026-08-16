# RTEMS dynamically loaded object coverage: results

2026-08-15. The plan (tcgcov `docs/RTEMS-DL.md`, built on
`docs/DYNAMIC-OBJECTS.md`) is **fully implemented and measured**. Fixtures:
RTEMS 7 riscv/mbv BSP (`amd-microblaze-v-generic`) under the PoC QEMU with
the tcgcov plugin; the stock `dl01`–`dl09` testsuite plus one custom
two-object fixture. Everything below runs with **zero RTEMS
modifications** except the explicitly optional R2 hooks.

## The two structural findings

1. **An RTEMS ".so" is ET_REL** — libdl rejects `ET_DYN` outright
   (`rtl-elf.c:1597`). Sections are placed independently at runtime, DWARF
   is 0-based per section: this is the Linux `.ko` problem tcgcov already
   solved, not the Linux `.so` problem.
2. **No new format was needed.** TCGCOV2's per-record tag carries a
   **loader generation** (bumped per completed load/unload) instead of an
   ASID — `metadata.ctx_kind` says which. The same mechanism that
   separated two same-base Linux processes separates two lifetimes of a
   reused RTEMS allocation.

## Stage results

| stage | deliverable | measured result |
|---|---|---|
| R0 sidecar map | `tcgcov modmap`: JSON map → per-(object,section) slices rebased to section offsets; overlapping windows refused loudly | dl01: 6/6 module addrs → `dl01-o1.c`, entry count 2, loop count 5 = argc 2+3 — matching the serial log's own output exactly |
| R1 dumper | `rtl-map-dump.c`: ~50 lines, application-side, public RTL API | true per-section runtime bases; cross-validated the plugin's map reconstruction (RTL places rap-text sections contiguously) |
| R3 generations | plugin `rtl_state=`/`rtl_debug=`: watches the loader's real `_rtld_debug_state()`, reads `r_map` from guest memory, snapshots per generation into artifact metadata | dl01 gen-1 slice with map **from the artifact's own metadata** (no GDB, no sidecar) reproduces R0's ground truth; dl02/05/07/08/09 all pass (up to 40 generations) |
| R4 reuse | per-generation attribution of reused address ranges | **dl09**: allocator returns identical addresses across 4 load/unload cycles; the 4 lifetimes of o1's window (14 execs each) stay separate, per-lifetime slices symbolize identically — a v1 artifact would blend to count 4, lifetimes unrecoverable. **Custom fixture**: size-identical payloads A/B, B lands at A's exact freed addresses; TB `0x8004a716` = count 7 → `pay_a.c` in gen 1, count 11 → `pay_b.c` in gen 3 |
| R2 hooks (optional) | branch `rtl-debugger-hooks` (d9ca18310d) on the GitLab fork — **no MR opened**: `rtems_rtl_debugger_load/unload(obj)`, 30 lines | GDB-verified: per-object hits, `CTOR_RUN` clear at the load hook, ctor output after. Plugin `rtl_load=` consumption measured: ctor execs move from generation 0 (empty map, unattributable) to generation 1 (map has the object) and `pay_a.c:14/16` symbolize — **coverage of code running inside `dlopen`** |

## What the artifact self-describes now

One QEMU invocation with `rtl_state=/rtl_debug=` produces a TCGCOV2
artifact whose metadata carries, per generation, every loaded object's
name and section layout — the module map travels *inside* the coverage
file. The host pipeline is `contexts`/`modmap`/`symbolize --section`,
all existing tools.

## Honest edges

* Constructor coverage needs the R2 hooks (`rtl_load=`); without them it
  is loudly unattributed, never silently wrong.
* First-fit only reuses a freed block when sizes fit the same way — the
  cross-object fixture engineers size-identity; real workloads may
  fragment differently (the generation tagging is correct either way).
* Guest struct offsets are the ILP32 `<link_elf.h>` layout, bounded
  walks; 64-bit targets need the planned DWARF-driven offsets.
* `rtl obj load` (shell path) is invisible to `_rtld_debug_state` — only
  the hooks cover it.

Evidence: tcgcov `examples/rtems-dl/` (all fixtures, scripts, measured
numbers), commits through c866c5d.
