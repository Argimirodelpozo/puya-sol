// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// CUSTOM battery: a signed sub-256 value (int128 / int24) must round-trip as
// canonical negative through EVERY read surface. Each function returns the
// value widened to int256 (or compares) so a missing sign-extension shows up
// as 2^128-5 (or 2^24-5) instead of -5.
contract C {
    int128 public stateVar;                       // public auto-getter
    mapping(uint256 => int128) m;                 // mapping value
    int128[] arr;                                 // dynamic array elem
    int128[3] farr;                               // fixed array elem
    struct S { uint64 tag; int128 x; }
    S sVar;                                       // struct field (covered before; battery completeness)
    mapping(uint256 => S) sm;                     // struct-in-mapping field
    int24 public smallVar;                        // sub-64-bit signed
    event E(int128 v);

    function setAll(int128 v) external {
        stateVar = v; m[1] = v; delete arr; arr.push(v); farr[1] = v;
        sVar = S(7, v); sm[2] = S(9, v); smallVar = int24(v);
    }
    function rState() external view returns (int256) { return int256(stateVar); }
    function rMap() external view returns (int256) { return int256(m[1]); }
    function rArr() external view returns (int256) { return int256(arr[0]); }
    function rFarr() external view returns (int256) { return int256(farr[1]); }
    function rStructField() external view returns (int256) { return int256(sVar.x); }
    function rStructInMap() external view returns (int256) { return int256(sm[2].x); }
    function rSmall() external view returns (int256) { return int256(smallVar); }
    // abi.decode round-trip of a signed value
    function rDecode(int128 v) external pure returns (int256) {
        bytes memory e = abi.encode(v);
        int128 back = abi.decode(e, (int128));
        return int256(back);
    }
    // calldata param straight-through (ABI boundary decode)
    function rParam(int128 v) external pure returns (int256) { return int256(v); }
    // tuple return through an internal call
    function two(int128 v) internal pure returns (int128, uint64) { return (v, 3); }
    function rTuple(int128 v) external pure returns (int256, uint64) {
        (int128 a, uint64 b) = two(v);
        return (int256(a), b);
    }
    // comparison semantics on each container (catches one-sided extension)
    function cmpAll(int128 v) external view returns (bool) {
        return stateVar == v && m[1] == v && arr[0] == v && farr[1] == v
            && sVar.x == v && sm[2].x == v && smallVar == int24(v);
    }
    // event arg (harness reads the log payload)
    function emitIt(int128 v) external { emit E(v); }
    // ternary + nested expression reads
    function rTernary(bool c) external view returns (int256) {
        return int256(c ? stateVar : m[1]);
    }

    // ── multireturn/single-return into struct fields (the old "DCE drop" +
    //    "call duplication" shapes; counting fn proves exactly-once) ──
    uint256 public cnt;
    struct P { uint128 a; uint128 b; }
    P p;
    function mk2() internal returns (uint128, uint128) { cnt++; return (11, 22); }
    function mk1(uint128 x) internal returns (uint128) { cnt++; return x + 1; }
    function destructure() external returns (uint128, uint128, uint256) {
        cnt = 0;
        (p.a, p.b) = mk2();          // the old drop shape
        return (p.a, p.b, cnt);      // expect (11, 22, 1)
    }
    function fieldCall() external returns (uint128, uint256) {
        p.a = 5; cnt = 0;
        p.a = mk1(p.a);              // the old duplication shape
        return (p.a, cnt);           // expect (6, 1)
    }
}
