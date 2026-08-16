"""Diffs a Python golden_record.py recording against a C++
`headless_runner --fixture` recording, tolerating small floating-point
differences (different libm/compiler, not a logic bug) but catching any
real divergence -- structural mismatches ("dead" vs alive, wrong era/pop,
positions off by more than the tolerance) print exactly where the two
implementations first disagree.

Usage: python diff_recordings.py py_out.csv cpp_out.csv [--tol 0.05]
"""
import argparse
import sys

NUM_TEAM_FIELDS = 6  # food, wood, oil, iron, era, pop
NUM_ENTITY_FIELDS = 4  # x, y, hp, alive (or "dead" sentinel x3 + 0)


def parse_row(line):
    return line.strip().split(",")


def compare_rows(py_row, cpp_row, tol):
    if py_row[0] != cpp_row[0]:
        return f"tick mismatch: py={py_row[0]} cpp={cpp_row[0]}"
    tick = py_row[0]
    if len(py_row) != len(cpp_row):
        return f"tick {tick}: field count mismatch ({len(py_row)} vs {len(cpp_row)})"
    for i in range(1, len(py_row)):
        a, b = py_row[i], cpp_row[i]
        if a == b:
            continue
        try:
            fa, fb = float(a), float(b)
        except ValueError:
            return f"tick {tick} field {i}: '{a}' != '{b}' (non-numeric)"
        if abs(fa - fb) > tol:
            return f"tick {tick} field {i}: {fa} vs {fb} (diff {abs(fa - fb):.4f} > tol {tol})"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("py_csv")
    ap.add_argument("cpp_csv")
    ap.add_argument("--tol", type=float, default=0.05)
    args = ap.parse_args()

    with open(args.py_csv) as f:
        py_rows = [parse_row(l) for l in f if l.strip()]
    with open(args.cpp_csv) as f:
        cpp_rows = [parse_row(l) for l in f if l.strip()]

    if len(py_rows) != len(cpp_rows):
        print(f"ROW COUNT MISMATCH: python={len(py_rows)} cpp={len(cpp_rows)}")
        sys.exit(1)

    for py_row, cpp_row in zip(py_rows, cpp_rows):
        mismatch = compare_rows(py_row, cpp_row, args.tol)
        if mismatch:
            print(f"FIRST DIVERGENCE: {mismatch}")
            sys.exit(1)

    print(f"MATCH: {len(py_rows)} snapshot rows agree within tolerance {args.tol}")


if __name__ == "__main__":
    main()
