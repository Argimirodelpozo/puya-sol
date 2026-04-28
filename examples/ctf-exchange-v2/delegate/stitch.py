"""Stitch F's bytecode into the lonely_chunk TEAL template.

Usage: python stitch.py <F.approval.bin> <orch.clear.bin> <output.teal>

The lonely_chunk template has %F_PAGE_N% / %CLEAR_PROGRAM% placeholders for
4 × 2048-byte slices of F + the orch's clear-state program. This script
reads F's compiled bytecode, slices it, and substitutes hex into the
template.

When F's binary is shorter than 8192 bytes, the trailing pages are zero-
padded so the inner-txn ApprovalProgramPages calls always submit 4 slices.
That keeps the template's instruction count fixed regardless of F's size.
"""
import sys
from pathlib import Path

PAGE = 2048
MAX_PAGES = 4


def main():
    if len(sys.argv) != 4:
        print("usage: stitch.py <F.approval.bin> <orch.clear.bin> <output.teal>")
        sys.exit(1)
    f_bin = Path(sys.argv[1]).read_bytes()
    clear_bin = Path(sys.argv[2]).read_bytes()
    out_path = Path(sys.argv[3])

    if len(f_bin) > PAGE * MAX_PAGES:
        print(f"F bytecode too big: {len(f_bin)} > {PAGE * MAX_PAGES}")
        sys.exit(1)

    # Pad F to 8192 bytes; AVM ApprovalProgramPages concatenates so trailing
    # zeros at the end act as no-ops past program end (or match orig length
    # if we trim — for now keep 4 fixed pages for instruction stability).
    padded = f_bin.ljust(PAGE * MAX_PAGES, b"\x00")
    pages = [padded[i * PAGE : (i + 1) * PAGE] for i in range(MAX_PAGES)]

    template_path = Path(__file__).parent / "lonely_chunk.teal.template"
    teal = template_path.read_text()

    for i, p in enumerate(pages):
        teal = teal.replace(f"%F_PAGE_{i}%", p.hex())
    teal = teal.replace("%CLEAR_PROGRAM%", clear_bin.hex())

    out_path.write_text(teal)
    print(f"wrote {out_path}; F={len(f_bin)}B clear={len(clear_bin)}B")


if __name__ == "__main__":
    main()
