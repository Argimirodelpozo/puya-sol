// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative STATEFUL fuzzer
// (fuzz_gen.py gen_stateful_contract → fuzz_state.py). A dynamic STORAGE array whose element is
// biguint-backed and NARROWER than 32 bytes (uint128/int128/uint160/uint192/... i.e. 64 < bits < 256)
// reports `a.length` = floor(total_element_bytes / 32) instead of the element COUNT — the length read
// uses a hardcoded 32-byte stride, not the element's actual width. The element DATA is stored and
// indexed correctly; only `.length` is wrong.
//   uint64[] / int64[] (<=64-bit, uint64-backed):   CORRECT (separate code path)
//   uint256[] / int256[] (32-byte element):         CORRECT (32/32 cancels — why the suite never caught it)
//   uint128[] x4 -> length 2 (= 4*16/32);  uint192[] x3 -> 2 (= floor(3*24/32))
// Frontend AWST is FAITHFUL (identical node structure to the working uint64[], only the element type
// differs) → this is a PUYA BACKEND bug (get_length in ir/builder/aggregates/sequence.py / the box
// arc4 array length path). xfail until fixed in puya.
contract WideDynamicArrayLength {
    uint128[] a;
    function push(uint128 v) external { a.push(v); }
    function len() external view returns (uint256) { return a.length; }
    function get(uint256 i) external view returns (uint128) { return a[i]; }
}
