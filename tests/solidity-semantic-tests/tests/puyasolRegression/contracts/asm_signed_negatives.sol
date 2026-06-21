// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (inline assembly / Yul
// ops). The Yul SIGNED ops are broken for NEGATIVE operands (POSITIVE operands are correct):
//   sdiv(a,3), a<0  → REVERTS (empty AVM panic; want signed quotient, e.g. sdiv(-128,3)=-42)
//   smod(a,3), a<0  → REVERTS (want signed remainder, e.g. smod(-128,3)=-2)
//   sar(2,a),  a<0  → WRONG (returns a 64-byte oversized value, not the sign-filled shift; want -32 for -128)
// ROOT CAUSE (found via the TEAL: the method sig is `sdivF(uint512)uint512`): an assembly-bodied function
// exposes its 256-bit params (and SIGNED returns) as arc4.uint512 (64 bytes), not uint256 — the asm body
// reinterprets the operand as `biguint` and puya maps biguint -> ARC4UIntN(512). So the int256 param is a
// 64-byte ABI value; the harness encodes a NEGATIVE int256 as a 512-bit two's complement (2^512-128), and
// negate256()'s `maxU256 - val` then UNDERFLOWS (val > 2^256-1) -> the empty `b-` panic. sar's "64-byte
// oversized" result is the same 64-byte operand flowing through. The sdiv/smod/sar LOGIC is correct for
// 256-bit operands; the bug is the uint512 ABI exposure (my earlier negate256-mod theory was wrong). Two
// gaps in the ABI derivation: (1) PARAM side — no param rewriter, so ALL 256-bit asm params (int256 AND
// uint256) expose as uint512; (2) RETURN side — ReturnRewriter Pass 2 (biguint return -> uint256) is gated
// `signedReturns.empty()`, so a SIGNED asm return stays biguint -> uint512 (uint256 asm returns are already
// correct). This is the asm-biguint-return "param side still open" + a signed-return gap. FRONTEND fix = a
// param rewriter (declare arc4.uint256, decode to biguint at entry) + un-gate the return pass for signed
// 256-bit. xfail until that lands. Verified in the semantic harness.
contract AsmSignedNegatives {
    function sdivF(int256 a) external pure returns (int256 r) { assembly { r := sdiv(a, 3) } }
    function smodF(int256 a) external pure returns (int256 r) { assembly { r := smod(a, 3) } }
    function sarF(int256 a)  external pure returns (int256 r) { assembly { r := sar(2, a) } }
}
