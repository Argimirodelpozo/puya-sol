// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the GENERATIVE differential fuzzer
// (fuzz_gen.py — random expression trees over sub-word types). A sub-word `<<`/`>>` by a constant
// (or any <=64-bit-typed) amount >= 64 took the raw uint64 shl/shr opcode, which FAILS for a shift
// >= 64 — but Solidity saturates `x << n` / `x >> n` to 0 (sign-fill for signed >>) when n >= the
// width and never reverts. The uint256/biguint path + the variable-uint256-amount path were already
// guarded; the fix routes ALL shifts through the biguint path.
contract SubwordShift {
    function shl16_64(uint16 a)  external pure returns (uint16) { return a << 64; }    // was REVERT → 0
    function shl16_256(uint16 a) external pure returns (uint16) { return a << 256; }   // was REVERT → 0
    function shl16_4(uint16 a)   external pure returns (uint16) { return a << 4; }      // in-range, unchanged
    function shr16_256(uint16 a) external pure returns (uint16) { return a >> 256; }   // was REVERT → 0
    function shl64_64(uint64 a)  external pure returns (uint64) { return a << 64; }    // was REVERT → 0
    function shlI16_256(int16 a) external pure returns (int16)  { return a << 256; }   // was REVERT → 0
    function shrI16_256(int16 a) external pure returns (int16)  { return a >> 256; }   // signed: sign-fill
    // shift as a SUB-expression: the biguint shift result must narrow to uint64 to compose with the
    // surrounding op (was a puya compile error "UInt64BinaryOperation expected uint64").
    function comp(uint64 a, uint64 b)  external pure returns (uint64) { return (a << 7) & b; }
    function compR(uint16 a, uint16 b) external pure returns (uint16) { return b | (a >> 3); }
}
