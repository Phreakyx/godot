---
type: Mistake
title: A source pin named the tag object, not its commit
description: The research recorded emhash v1.3.0 as 2f6301b972, the SHA of the annotated tag object; fetching it checked out the tag's commit e84fa51009, so the runner's rev-parse check refused the pin.
tags: [mistake, benchmark, git, supply-chain]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T00:00:00Z }
sources:
  - id: bench-py
    resource: ../../devtools/bench/bench.py
    title: PINS and fetch_git (the rev-parse check)
---

# What happened

- The container micro-benchmark controls are pinned by commit in `PINS`
  (`devtools/bench/bench.py`). For emhash the research took the SHA that
  `refs/tags/v1.3.0` points to, `2f6301b972`. emhash's tags are annotated:
  that SHA is a tag object, and `git ls-remote` lists the commit as
  `refs/tags/v1.3.0^{}`, `e84fa51009`.
- `git fetch <sha>` of a tag object works and `FETCH_HEAD` is the peeled
  commit, so the checkout looked fine. `fetch_git` then compares
  `git rev-parse HEAD` with the pin and refused: HEAD `e84fa51009` is not
  `2f6301b972`. unordered_dense and gtl use lightweight tags, where the
  tag ref is the commit, so they passed.

# Rule

- Pin the commit: take the `^{}` line of `git ls-remote <url> refs/tags/<tag>*`
  when there is one, else the tag line. Check that `git cat-file -t <sha>`
  prints `commit`.
- Keep the `rev-parse HEAD` check after every fetch. It is what caught
  this; without it the pin would have read as a commit nobody checked.
- For a release archive, compare the sha256 of the first download with a
  second source before recording it (GitHub's API lists a `digest` per
  release asset), not only with itself.
