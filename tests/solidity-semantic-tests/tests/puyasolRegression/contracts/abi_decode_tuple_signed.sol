// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the abi-round-trip fuzz probe.
// abi.decode of a TUPLE with a signed sub-64 element (int8/16/32), returned DIRECTLY, failed to COMPILE:
// the decode produces the native tuple (int16 -> uint64) but a multi-return ABI function widens each
// signed sub-64 element to biguint (256-bit two's complement for the ARC4 uint256 encoding), and the
// per-element widening in ReturnRewriter only handled tuple LITERALS (`return (a,b)`), not an opaque
// tuple-producing expression. So the decoded uint64 element mismatched the biguint return slot
// (`invalid return type [biguint, uint64] expected [biguint, biguint]`). FIX: bind the opaque tuple to a
// temp and rebuild it as a literal with the signed sub-64 elements sign-extended. (Single int16 return,
// unsigned sub-word tuples, and int128 tuples already compiled.)
contract C {
    // direct tuple return — compiling these IS the regression guard for the compile error.
    function rt2(uint128 a, int16 b) external pure returns (uint128, int16) {
        return abi.decode(abi.encode(a, b), (uint128, int16));
    }
    function rt3(int16 a, int32 b, int8 c) external pure returns (int16, int32, int8) {
        return abi.decode(abi.encode(a, b, c), (int16, int32, int8));
    }
    // on-chain identity (clean bool) — guards the VALUES round-trip (incl. negatives).
    function id2(uint128 a, int16 b) external pure returns (bool) {
        (uint128 ra, int16 rb) = abi.decode(abi.encode(a, b), (uint128, int16));
        return ra == a && rb == b;
    }
    function id3(int16 a, int32 b, int8 c) external pure returns (bool) {
        (int16 ra, int32 rb, int8 rc) = abi.decode(abi.encode(a, b, c), (int16, int32, int8));
        return ra == a && rb == b && rc == c;
    }
    function id2u(uint128 a, uint16 b) external pure returns (bool) { // unsigned control
        (uint128 ra, uint16 rb) = abi.decode(abi.encode(a, b), (uint128, uint16));
        return ra == a && rb == b;
    }
}
