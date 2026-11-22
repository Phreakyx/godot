---
type: Decision
title: ALLOC-02 - mimalloc against the HashMap node pool (four-way compare)
description: The rules of the ALLOC-02 head-to-head, stated before any measurement - control, mimalloc only, pool only and both, same source, editor and template_release, every suite workload plus the micro tier and the tiny-maps case; target rows per compiler (MSVC script:script, GCC and Clang scene:instance:10000, template_release), the mechanical pass rule (bench.py verdict), the count rules, the tiny-maps report and the baseline rule for every later hash candidate. Also the exploratory Linux-only override variant (FORK_BACKEND_ALLOCATOR=3), reported next to the series and never part of the baseline. Results and the decision come from the MSVC and the Linux sessions.
tags: [core, allocation, mimalloc, hashmap, pool, benchmark, decision, phase-3]
status: draft
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T00:00:00Z }
sources:
  - id: shares
    resource: family-shares.md
    title: Container family shares and the allocation target per compiler
  - id: floors
    resource: benchmark-noise-floors.md
    title: A/A noise floors on the i9-14900K (MSVC, GCC, Clang)
  - id: protocol
    resource: ../tooling/benchmark-protocol.md
    title: Benchmark protocol (accept, regression, count and skip rules; commands)
  - id: mimalloc
    resource: mimalloc-allocator.md
    title: The allocator candidate (FORK_BACKEND_ALLOCATOR=1)
  - id: pool
    resource: hashmap-node-pool.md
    title: The pool candidate (FORK_BACKEND_HASHMAP_POOL=1)
---

# What is compared

Four builds of one source (`71b2819fec` is the previous side of every
series):

| Side | Macros | bench variant |
| --- | --- | --- |
| control | both 0 | `bench` |
| alloc | `FORK_BACKEND_ALLOCATOR=1` | `bench.alloc` |
| pool | `FORK_BACKEND_HASHMAP_POOL=1` | `bench.pool` |
| both | `FORK_BACKEND_ALLOCATOR=1 FORK_BACKEND_HASHMAP_POOL=1` | `bench.both` |

Each candidate binary and its dev tests build passed
`--test --test-case="*Fork*"` before measuring, the new
`[Fork][Backends] Allocator scope` case included: with value 1 a raw
`malloc` block is not in mimalloc's heap, so the candidate overrides nothing.

# Rules stated before measuring

Written and committed before the first ALLOC-02 series; not changed after
it starts.

**Series, per compiler.** For each name in `alloc`, `pool`, `both`:

```
python devtools/bench/bench.py compare --name <name> --defines "<macros>" --prev 71b2819fec \
    --target <target workload> --workloads all
python devtools/bench/bench.py micro --name <name> --defines "<macros>" --prev 71b2819fec
```

against the same-source control with both macros 0, on editor and
template_release. On Linux add `--compiler gcc` or `--compiler clang`.

**Target rows (D-06)**, the workload with the largest allocation share per
compiler ([shares](/project/family-shares.md)), each with its A/A floor
([floors](/project/benchmark-noise-floors.md)):

| Compiler | Target row | Floor (`frame_ms_p50`) |
| --- | --- | --- |
| MSVC | `script:script@template_release` | 6.94% |
| GCC | `scene:instance:10000@template_release` | 1.33% |
| Clang | `scene:instance:10000@template_release` | 8.31% |

Every other row of the series gets the regression rule (time and
`peak_ws_mb`).

**Pass.** A series passes when

```
python devtools/bench/bench.py verdict --series <dir> --rows <the compiler's target row>
```

prints PASS: the target row reads ACCEPT and no row reads REGRESSION. A
micro series never passes or fails a candidate alone (protocol).

**Counts.**

- The alloc series must report allocation counts identical to the control
  on every workload: `Memory::` calls are unchanged (D-03), so any
  difference is a bug to fix, not a result.
- The pool and both series must read NO-INCREASE. Any ALLOC-INCREASE is
  explained here or rejects the series.

**Tiny maps (D-05).** For the pool and both micro series, the `live_bytes`
and `allocs` of every `*_tiny_*` row are reported against the control. If
a series passes on time but raises tiny-map `live_bytes`, this entry says
so next to the decision.

**Baseline rule**, applied per compiler, then across compilers:

- A macro is in the baseline only when its single series passes.
- When both singles pass and the both series passes, the baseline is both.
- When both singles pass but both fails, the baseline is the single with
  the more negative target median.
- When neither single passes, the baseline is empty.
- On MSVC the result is provisional (D-07). The final baseline (from the
  Linux session) keeps a macro only when its series passes on MSVC, GCC and
  Clang, and applies the both rule with the worst compiler's target median.

**n and budget.** The runner's planned rounds; each series stays under the
30-minute budget.

**Noise of a baseline control.** The A/A floors were measured on the
default control. If a series shows a control whose own rounds vary clearly
more than the floor, the series is repeated once before its verdict is
used, and the spread is recorded here.

## Exploratory, outside the baseline rule (D-17)

The override variant `FORK_BACKEND_ALLOCATOR=3` makes mimalloc the
process-wide malloc (Linux only; every other compile stops at an `#error`).
It runs in the Linux session on GCC and Clang after the four-way series,
and only once its full `--test`, the Fork cases (a `malloc`, an
`operator new` and a glibc-internal `realpath` block inside mimalloc's
heap) and the smoke test pass:

```
python3 devtools/bench/bench.py compare --name override --defines "FORK_BACKEND_ALLOCATOR=3" \
    --prev 71b2819fec --target scene:instance:10000 --workloads all --compiler <gcc|clang>
```

against the same-source control. Its verdict reading and target median are
reported next to the alloc series (same control), so the difference shows
what the allocations outside `Memory::` add. Its counts must equal the
control's (`Memory::` calls unchanged, D-03). It never enters the baseline
rule, is never accepted and never becomes a default in this phase.

# Results

Not measured yet (MSVC four-way compare, then the Linux session).

# Decision

Not made yet.
