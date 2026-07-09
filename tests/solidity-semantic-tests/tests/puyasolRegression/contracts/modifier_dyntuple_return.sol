// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Two cross-contract-return wire-type classes, found by the differential fuzzer and fixed in
// ReturnRewriter (fable-review-2 D2). Kept as a SMALL standalone pair: puya-sol embeds the callee's
// bytecode into the caller's program, so a big shared fixture blows the deploy page budget.
//
// (a) biguint element in a DYNAMIC-element tuple `(uint128, bytes)`. ReturnRewriter Pass 3 used to
//     wrap biguint elements only in ALL-STATIC tuples (the `allStatic` guard), so a tuple with a
//     dynamic element left the uint128 unwrapped → puya named it "uint512" while the caller names
//     "uint128" → selector mismatch → UNCONDITIONAL revert. Fixed by wrapping biguint in ANY tuple.
//
// (b) MODIFIER'D (chain-lowered) returns. The modifier chain threads NATIVE (biguint) return values
//     through its subs; the OUTER dispatch return used to publish the bare biguint as "uint512" while
//     callers name the declared width. Fixed by encodeChainDispatchReturn (encodes the outer return)
//     + threading the promoted returnType through buildModifierChain (a fresh map() gives int64→uint64
//     and mismatches the biguint body → "Tuple type mismatch"). Covers single unsigned-wide, single
//     signed sub-word, unsigned tuple, and signed tuple — with negatives.
contract Cee {
    uint256 public hits;
    modifier bump() { hits += 1; _; }
    function dbig(uint256 a) external pure returns (uint128, bytes memory) {
        bytes memory b = new bytes(2); b[0] = 0xaa; b[1] = 0xbb; return (uint128(a), b);
    }
    function mu128(uint256 a) external bump returns (uint128) { return uint128(a); }
    function mi64(int256 a)  external bump returns (int64)   { return int64(a); }
    function mtup(uint256 a) external bump returns (uint64, uint128) { return (uint64(a), uint128(a)); }
    function mstup(int256 a) external bump returns (int64, int128) { return (int64(a), int128(a)); }
}
contract Caller {
    Cee c;
    constructor() { c = new Cee(); }
    // (a) dynamic-element tuple with a biguint element. 0xaa=170, len 2.
    function gdbig(uint256 a) external returns (uint256) { (uint128 x, bytes memory b) = c.dbig(a); return uint256(x) + uint256(uint8(b[0])) + b.length; }
    // (b) modifier'd returns; widen+offset for a clean positive observable on the signed ones.
    function gmu128(uint256 a) external returns (uint256) { return uint256(c.mu128(a)); }
    function gmi64(int256 a)  external returns (int256)  { return int256(c.mi64(a)) + 1000; }
    function gmtup(uint256 a) external returns (uint256) { (uint64 x, uint128 y) = c.mtup(a); return uint256(x) + uint256(y); }
    function gmstup(int256 a) external returns (int256) { (int64 x, int128 y) = c.mstup(a); return int256(x) + int256(y) + 1000; }
}
