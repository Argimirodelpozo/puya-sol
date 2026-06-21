// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (inline assembly / Yul
// ops). The Yul SIGNED ops are broken for NEGATIVE operands (POSITIVE operands are correct):
//   sdiv(a,3), a<0  → REVERTS (empty AVM panic; want signed quotient, e.g. sdiv(-128,3)=-42)
//   smod(a,3), a<0  → REVERTS (want signed remainder, e.g. smod(-128,3)=-2)
//   sar(2,a),  a<0  → WRONG (returns a 64-byte oversized value, not the sign-filled shift; want -32 for -128)
// Two sub-causes identified (assembly/SignedOps.cpp): (1) negate256() computes (2^256-1)-val+1 WITHOUT
// `mod 2^256`, so negate256(0)=2^256 (33 bytes) trips the biguint->arc4.uint256 len<=32 assert — a real
// bug, but adding the mod did NOT resolve the panic, so (2) a SEPARATE opcode-level panic remains
// (suspect a `b-` underflow or len assert in the abs/negate path; the revert is an empty RawRevert so it
// needs an AVM trace to pin). sar's 64-byte result is a third thread (fillMask/OR not masked to 256 bits).
// FRONTEND (the Yul handlers); xfail until pinned + fixed. Verified in the semantic harness.
contract AsmSignedNegatives {
    function sdivF(int256 a) external pure returns (int256 r) { assembly { r := sdiv(a, 3) } }
    function smodF(int256 a) external pure returns (int256 r) { assembly { r := smod(a, 3) } }
    function sarF(int256 a)  external pure returns (int256 r) { assembly { r := sar(2, a) } }
}
