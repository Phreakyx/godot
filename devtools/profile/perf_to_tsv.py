#!/usr/bin/env python3
"""`perf script` output to the sampler.exe TSV format, so analyze.py reads Linux profiles (README.md).

  perf script -F tid,comm,ip,sym,dso,srcline --inline --full-source-path | python perf_to_tsv.py > x.tsv

A sample is a header line (comm and tid; without a call graph also its one frame), then with a call
graph (perf record -g) one tab-indented line per frame, each followed by its srcline line. Frames
marked "(inlined)" join the next real frame's segment, innermost first, as sampler.exe writes inline
frames; each real frame closes a segment, so callers become the later segments. Output rows are
`#thread<TAB>tid<TAB>comm` and `count<TAB>tid<TAB>module<TAB>frames`, identical stacks merged.
"""

import io
import os
import re
import sys
from collections import Counter

SRCLINE = re.compile(r"^\s+(?P<file>[^\s()]+):(?P<line>\d+|\?+)(?:\s+\(discriminator \d+\))?(?P<inlined>\s+\(inlined\))?\s*$")
FRAME = re.compile(r"^(?:[0-9a-f]+\s+)?(?P<sym>.*?)(?:\+0x[0-9a-f]+)?(?:\s+\((?P<dso>[^()]*)\))?(?P<inlined>\s+\(inlined\))?\s*$")
# ponytail: a comm that ends in a number token is taken whole (greedy); a frame whose ip is all decimal digits on a
# header line (no call graph) would be read as the tid. Checked against real output in 02-12.
# perf 7.3 ends a call-graph header with a space after the tid ("godot.linuxbsd.  137569 "), hence the trailing \s*.
HEADER = re.compile(r"^\s*(?P<comm>.*\S)\s+(?P<tid>\d+)(?:\s+(?P<frame>\S.*?))?\s*$")


def convert(lines):
    threads, stacks = {}, Counter()
    sample = None  # [tid, frames]; a frame is [symbol, file:line or "", inlined, dso]

    def add_frame(text):
        m = FRAME.match(text.strip())
        sym = m.group("sym").split("@")[0].replace("\t", " ") or "?"  # drop symbol versions (malloc@@GLIBC_2.2.5)
        sample[1].append([sym, "", bool(m.group("inlined")), m.group("dso")])

    def flush():
        if not sample or not sample[1]:
            return
        segments, current, module = [], [], None
        for sym, where, inlined, dso in sample[1]:
            current.append(f"{sym}@{where}" if where else sym)
            if module is None and dso:
                module = os.path.basename(dso)
            if not inlined:
                segments.append(tuple(current))
                current = []
        if current:
            segments.append(tuple(current))
        stacks[(sample[0], module or "?", tuple(segments))] += 1

    for raw in lines:
        line = raw.rstrip("\n")
        if not line.strip():
            flush()
            sample = None
            continue
        if line.startswith("\t") and sample is not None:
            add_frame(line)
            continue
        m = SRCLINE.match(line)
        if m and sample is not None and sample[1]:
            frame = sample[1][-1]
            if not m.group("file").startswith("?"):
                frame[1] = f"{m.group('file')}:{m.group('line')}"
            frame[2] = frame[2] or bool(m.group("inlined"))
            continue
        m = HEADER.match(line)
        if not m:
            continue  # a perf warning or an unknown line
        flush()
        sample = [m.group("tid"), []]
        threads.setdefault(m.group("tid"), m.group("comm"))
        if m.group("frame"):
            add_frame(m.group("frame"))
    flush()
    out = [f"#thread\t{tid}\t{comm}" for tid, comm in threads.items()]
    for (tid, module, segments), count in stacks.items():
        out.append(f"{count}\t{tid}\t{module}\t" + "\t".join(segments[0]) +
                   "".join("\t|" + "\t".join(segment) for segment in segments[1:]))
    return out


def main():
    lines = io.TextIOWrapper(sys.stdin.buffer, encoding="utf-8", errors="replace")
    sys.stdout.write("\n".join(convert(lines)) + "\n")


if __name__ == "__main__":
    main()
