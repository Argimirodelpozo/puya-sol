#!/usr/bin/env python3
"""
TEAL post-processor that replaces the unrolled PoseidonT3.hash subroutine
with a compact looped version that reads round constants from box storage.

This reduces the program from ~14KB to ~3.5KB by:
1. Removing 197 inline pushbytes (6,048 bytes of round constants)
2. Replacing 4,710 lines of unrolled rounds with ~200 lines of loop code
3. Reading round constants from a box named "prc" using box_extract

The test must pre-populate the "prc" box with the packed constants blob
before calling any methods that use PoseidonT3.hash.
"""
import re
import sys


def extract_constants(lines: list[str], start: int, end: int) -> list[bytes]:
    """Extract all pushbytes constants from PoseidonT3.hash subroutine."""
    constants = []
    for i in range(start, end):
        stripped = lines[i].strip()
        if stripped.startswith("pushbytes 0x"):
            hex_val = stripped.split("0x")[1]
            # Pad to 32 bytes if shorter (leading zero was stripped)
            hex_val = hex_val.zfill(64)
            constants.append(bytes.fromhex(hex_val))
    return constants


def find_poseidon_bounds(lines: list[str]) -> tuple[int, int]:
    """Find start and end line indices of PoseidonT3.hash subroutine."""
    start = -1
    end = -1
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped == "PoseidonT3.hash:" or stripped.startswith("PoseidonT3.hash:"):
            start = i
        elif start >= 0 and stripped == "retsub" and end < 0:
            end = i + 1
            break
    return start, end


def build_looped_poseidon(constants: list[bytes]) -> list[str]:
    """Build a compact PoseidonT3.hash using box_extract for round constants.

    Round structure (Poseidon T3, BN254):
    - 2 initial absorption constants (inline)
    - Round 0: special first full round (6 constants inline)
    - Rounds 1-3: full rounds (3 iter, constants from box)
    - Rounds 4-60: partial rounds (57 iter, constants from box)
    - Rounds 61-63: full rounds (3 iter, constants from box)
    - Round 64: final full round (no constants)

    Box "prc" layout: rounds 1-63, 3 constants per round, 32 bytes each
    Total: 63 * 3 * 32 = 6,048 bytes
    """
    # Separate constants by role
    initial_absorb = constants[0:2]  # 2 constants
    round0_consts = constants[2:8]  # 6 constants for round 0
    # Constants 8..197 = rounds 1..63 (3 per round) = 189 constants
    round_consts = constants[8:]  # 189 constants for rounds 1-63

    # Build packed blob for box storage (returned separately)
    blob = b"".join(round_consts)
    assert len(blob) == 189 * 32 == 6048, f"Expected 6048 bytes, got {len(blob)}"

    # Field prime F is bytec_0
    F = "bytec_0 // F"
    # MDS matrix constants (already in bytec pool)
    M = [
        ["bytec 7", "bytec_1", "bytec_2"],   # row 0
        ["bytec 8", "bytec_3", "bytec 4"],    # row 1
        ["bytec 9", "bytec 5", "bytec 6"],    # row 2
    ]

    def hex_const(b: bytes) -> str:
        return "0x" + b.hex()

    lines = []
    lines.append("// /home/argi/AlgorandFoundation/SideProjects/puya-sol/puya-sol/examples/zk-kit/contracts/test/LeanIMTTest.sol.PoseidonT3.hash(_param0: uint64) -> bytes:")
    lines.append("PoseidonT3.hash:")

    # --- Preamble: load inputs ---
    lines.append("    // Load inputs from memory parameter")
    lines.append("    frame_dig -1")
    lines.append("    loads")
    lines.append("    dup")
    lines.append("    extract 0 64")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append(f"    pushbytes {hex_const(initial_absorb[0])}")
    lines.append("    b+")
    lines.append("    swap")
    lines.append("    extract 64 64")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append(f"    pushbytes {hex_const(initial_absorb[1])}")
    lines.append("    b+")
    lines.append("    // Stack: state0 state1 (state2 = 0 implicit)")

    # --- Round 0: special first full round (2 S-boxes, state2=0) ---
    lines.append("    // Round 0: special (state2 = 0)")
    # S-box on state0: x^5 = x * (x^2)^2
    lines.append("    dig 1")
    lines.append("    dig 2")
    lines.append(f"    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    dup")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    uncover 2")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    # S-box on state1
    lines.append("    dig 1")
    lines.append("    dig 2")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    dup")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    uncover 2")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    # MDS for round 0 (state2=0 means we only need state0 and state1 terms)
    # new_state0 = M[0][0]*s0 + M[0][1]*s1 + rc0_0
    # Note: round 0 uses different MDS column ordering (bytec_1..6 for 2-state input)
    # Replicate the exact pattern from the original
    lines.append("    // Round 0 MDS (state2 = 0)")
    lines.append("    dup")
    lines.append(f"    bytec_1")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    dig 2")
    lines.append(f"    bytec_2")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append(f"    pushbytes {hex_const(round0_consts[0])}")
    lines.append("    b+")
    # new_state1
    lines.append("    dig 1")
    lines.append(f"    bytec_3")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    dig 3")
    lines.append(f"    bytec 4")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append(f"    pushbytes {hex_const(round0_consts[1])}")
    lines.append("    b+")
    # Now include 3-element MDS with round 0's additional constants
    # new_state0 complete = M00*s0 + M10*s1 + rc[2]  (using the original's numbering)
    lines.append("    uncover 2")
    lines.append(f"    bytec 5")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    uncover 3")
    lines.append(f"    bytec 6")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append(f"    pushbytes {hex_const(round0_consts[2])}")
    lines.append("    b+")
    # Round 0 produces 3 state values using 3x2 MDS (not 3x3)
    # The actual Poseidon implementation adds separate constant sets
    # Let me inline the remaining round 0 constants
    lines.append("    // Round 0 second MDS half")
    lines.append("    dig 2")
    lines.append("    dig 3")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    dup")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    uncover 3")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    # The 3 remaining round 0 constants (indices 3,4,5)
    lines.append("    dup")
    lines.append(f"    {M[0][0]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    dig 3")
    lines.append(f"    {M[0][1]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append("    dig 2")
    lines.append(f"    {M[0][2]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append(f"    pushbytes {hex_const(round0_consts[3])}")
    lines.append("    b+")
    # MDS row 1
    lines.append("    dig 1")
    lines.append(f"    {M[1][0]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    dig 4")
    lines.append(f"    {M[1][1]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append("    dig 3")
    lines.append(f"    {M[1][2]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append(f"    pushbytes {hex_const(round0_consts[4])}")
    lines.append("    b+")
    # MDS row 2
    lines.append("    uncover 2")
    lines.append(f"    {M[2][0]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    uncover 4")
    lines.append(f"    {M[2][1]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append("    uncover 3")
    lines.append(f"    {M[2][2]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append(f"    pushbytes {hex_const(round0_consts[5])}")
    lines.append("    b+")
    lines.append("    // Stack: state0 state1 state2")

    # --- Save states to scratch, prepare for loops ---
    lines.append("    // Store states in scratch space for loop")
    lines.append("    store 3  // state2")
    lines.append("    store 2  // state1")
    lines.append("    store 1  // state0")
    lines.append("    pushint 0")
    lines.append("    store 4  // box offset")

    # --- Full rounds 1-3 (loop 3 times) ---
    lines.append("    // Full rounds 1-3")
    lines.append("    pushint 3")
    lines.append("poseidon_full_loop_1:")
    lines.append("    callsub poseidon_full_round")
    lines.append("    pushint 1")
    lines.append("    -")
    lines.append("    dup")
    lines.append("    bnz poseidon_full_loop_1")
    lines.append("    pop")

    # --- Partial rounds 4-60 (loop 57 times) ---
    lines.append("    // Partial rounds 4-60")
    lines.append("    pushint 57")
    lines.append("poseidon_partial_loop:")
    lines.append("    callsub poseidon_partial_round")
    lines.append("    pushint 1")
    lines.append("    -")
    lines.append("    dup")
    lines.append("    bnz poseidon_partial_loop")
    lines.append("    pop")

    # --- Full rounds 61-63 (loop 3 times) ---
    lines.append("    // Full rounds 61-63")
    lines.append("    pushint 3")
    lines.append("poseidon_full_loop_2:")
    lines.append("    callsub poseidon_full_round")
    lines.append("    pushint 1")
    lines.append("    -")
    lines.append("    dup")
    lines.append("    bnz poseidon_full_loop_2")
    lines.append("    pop")

    # --- Final round 64: S-box(3) + MDS, no constants ---
    lines.append("    // Final round 64 (no constants)")
    lines.append("    load 1")
    lines.append("    load 2")
    lines.append("    load 3")
    # S-box on all 3 states
    for _ in range(3):
        lines.append("    dig 2")
        lines.append("    dig 3")
        lines.append("    b*")
        lines.append(f"    {F}")
        lines.append("    b%")
        lines.append("    dup")
        lines.append("    b*")
        lines.append(f"    {F}")
        lines.append("    b%")
        lines.append("    uncover 3")
        lines.append("    b*")
        lines.append(f"    {F}")
        lines.append("    b%")
    # Final MDS (no round constants)
    # new_state0 = M00*s0 + M10*s1 + M20*s2
    lines.append("    dup")
    lines.append(f"    {M[0][0]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    uncover 2")
    lines.append(f"    {M[0][1]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append("    dig 1")
    lines.append(f"    {M[0][2]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    # Return only state0
    lines.append(f"    {F}")
    lines.append("    b%")
    # Clean up remaining state1, state2
    lines.append("    swap")
    lines.append("    pop")
    lines.append("    swap")
    lines.append("    pop")
    lines.append("    retsub")

    # --- Full round subroutine ---
    lines.append("")
    lines.append("poseidon_full_round:")
    lines.append("    load 1")
    lines.append("    load 2")
    lines.append("    load 3")
    # S-box on all 3 states
    for _ in range(3):
        lines.append("    dig 2")
        lines.append("    dig 3")
        lines.append("    b*")
        lines.append(f"    {F}")
        lines.append("    b%")
        lines.append("    dup")
        lines.append("    b*")
        lines.append(f"    {F}")
        lines.append("    b%")
        lines.append("    uncover 3")
        lines.append("    b*")
        lines.append(f"    {F}")
        lines.append("    b%")
    # MDS + round constants from box
    _emit_mds_with_box(lines, M, F)
    lines.append("    store 3")
    lines.append("    store 2")
    lines.append("    store 1")
    lines.append("    retsub")

    # --- Partial round subroutine ---
    lines.append("")
    lines.append("poseidon_partial_round:")
    lines.append("    load 1")
    lines.append("    load 2")
    lines.append("    load 3")
    # S-box on state0 only
    lines.append("    dig 2")
    lines.append("    dig 3")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    dup")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    uncover 3")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    # MDS + round constants from box
    _emit_mds_with_box(lines, M, F)
    lines.append("    store 3")
    lines.append("    store 2")
    lines.append("    store 1")
    lines.append("    retsub")

    return lines, blob


def _emit_mds_with_box(lines: list[str], M: list[list[str]], F: str):
    """Emit MDS matrix multiply with round constants loaded from box "prc".

    Stack on entry: state0 state1 state2
    Box offset in scratch 4, incremented by 96 (3 * 32).
    """
    # Load 3 round constants from box
    # rc0 at scratch4, rc1 at scratch4+32, rc2 at scratch4+64
    lines.append("    // Load round constants from box")
    lines.append('    pushbytes "prc"')
    lines.append("    load 4")
    lines.append("    pushint 32")
    lines.append("    box_extract  // rc0")
    lines.append('    pushbytes "prc"')
    lines.append("    load 4")
    lines.append("    pushint 32")
    lines.append("    +")
    lines.append("    pushint 32")
    lines.append("    box_extract  // rc1")
    lines.append('    pushbytes "prc"')
    lines.append("    load 4")
    lines.append("    pushint 64")
    lines.append("    +")
    lines.append("    pushint 32")
    lines.append("    box_extract  // rc2")
    # Update offset: scratch4 += 96
    lines.append("    load 4")
    lines.append("    pushint 96")
    lines.append("    +")
    lines.append("    store 4")
    # Stack: state0 state1 state2 rc0 rc1 rc2
    # Store rc's in scratch 5,6,7
    lines.append("    store 7  // rc2")
    lines.append("    store 6  // rc1")
    lines.append("    store 5  // rc0")

    # MDS row 0: new_state0 = M00*s0 + M10*s1 + M20*s2 + rc0
    lines.append("    // MDS row 0")
    lines.append("    dup")
    lines.append(f"    {M[0][0]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    dig 3")
    lines.append(f"    {M[0][1]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append("    dig 2")
    lines.append(f"    {M[0][2]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append("    load 5")
    lines.append("    b+")

    # MDS row 1: new_state1 = M01*s0 + M11*s1 + M21*s2 + rc1
    lines.append("    // MDS row 1")
    lines.append("    dig 1")
    lines.append(f"    {M[1][0]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    dig 4")
    lines.append(f"    {M[1][1]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append("    dig 3")
    lines.append(f"    {M[1][2]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append("    load 6")
    lines.append("    b+")

    # MDS row 2: new_state2 = M02*s0 + M12*s1 + M22*s2 + rc2
    lines.append("    // MDS row 2")
    lines.append("    uncover 2")
    lines.append(f"    {M[2][0]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    uncover 4")
    lines.append(f"    {M[2][1]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append("    uncover 3")
    lines.append(f"    {M[2][2]}")
    lines.append("    b*")
    lines.append(f"    {F}")
    lines.append("    b%")
    lines.append("    b+")
    lines.append("    load 7")
    lines.append("    b+")
    # Stack: new_state0 new_state1 new_state2


def optimize_teal(input_path: str, output_path: str | None = None) -> bytes:
    """Optimize PoseidonT3.hash in TEAL file and return round constants blob."""
    with open(input_path) as f:
        lines = f.readlines()

    start, end = find_poseidon_bounds(lines)
    if start < 0:
        print("PoseidonT3.hash not found, no optimization needed")
        return b""

    print(f"Found PoseidonT3.hash at lines {start+1}-{end}")

    # Extract constants
    constants = extract_constants(lines, start, end)
    print(f"Extracted {len(constants)} round constants")

    # Build replacement
    new_lines, blob = build_looped_poseidon(constants)
    print(f"Replacement: {len(new_lines)} lines")
    print(f"Round constants blob: {len(blob)} bytes")

    # Replace in TEAL
    # Find the comment line before the subroutine (usually a source location comment)
    comment_start = start
    if start > 0 and lines[start - 1].strip().startswith("//"):
        comment_start = start - 1

    result = lines[:comment_start] + [line + "\n" for line in new_lines] + lines[end:]

    out_path = output_path or input_path
    with open(out_path, "w") as f:
        f.writelines(result)

    print(f"Wrote optimized TEAL to {out_path}")
    print(f"Original: {len(lines)} lines, Optimized: {len(result)} lines")

    return blob


def write_blob(blob: bytes, output_path: str):
    """Write the round constants blob to a binary file."""
    with open(output_path, "wb") as f:
        f.write(blob)
    print(f"Wrote {len(blob)} bytes to {output_path}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: optimize_poseidon.py <teal_file> [output_teal] [blob_file]")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else input_file
    blob_file = sys.argv[3] if len(sys.argv) > 3 else input_file.replace(".teal", ".poseidon_rc.bin")

    blob = optimize_teal(input_file, output_file)
    if blob:
        write_blob(blob, blob_file)
