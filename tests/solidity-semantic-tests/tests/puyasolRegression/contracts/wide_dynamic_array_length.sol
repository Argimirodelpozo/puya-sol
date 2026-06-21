// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative STATEFUL fuzzer
// (fuzz_gen.py gen_stateful_contract -> fuzz_state.py). A dynamic STORAGE array's `.length` read
// floor(total_bytes / 32) — a hardcoded 32-byte stride — instead of the element COUNT, whenever the
// element's encoded width != 32. DATA was stored/indexed correctly; only `.length` was wrong.
//   uint128[] x3 -> 1 (= 3*16/32);  uint160[] x3 -> 1 (= floor(3*20/32));  uint8[] x3 -> 0 (= 3*1/8)
//   uint256[] / uint64[]: already correct (32/32, 8/8 cancel) — why the o.g. suite never caught it.
// FRONTEND bug, FIXED: SolLengthAccess.cpp derived the box divisor via map()+mapToARC4Type, which
// erases sub-256 integer widths to biguint (-> 32). push/index use the width-preserving mapSolTypeToARC4
// (uint128 -> arc4.uint128 -> 16). The two are now aligned, so `.length` divides by the real stride.
contract WideDynamicArrayLength {
    uint128[] a;     // 16-byte stride (was /32)
    uint160[] b;     // 20-byte stride (was /32)
    uint32[] c;      //  4-byte stride (was /8)
    uint8[] d;       //  1-byte stride (was /8)
    uint256[] e;     // 32-byte stride (control: always worked)

    function push(uint128 v) external { a.push(v); }
    function len() external view returns (uint256) { return a.length; }
    function get(uint256 i) external view returns (uint128) { return a[i]; }

    function pushB(uint160 v) external { b.push(v); }
    function lenB() external view returns (uint256) { return b.length; }
    function pushC(uint32 v) external { c.push(v); }
    function lenC() external view returns (uint256) { return c.length; }
    function pushD(uint8 v) external { d.push(v); }
    function lenD() external view returns (uint256) { return d.length; }
    function pushE(uint256 v) external { e.push(v); }
    function lenE() external view returns (uint256) { return e.length; }
}
