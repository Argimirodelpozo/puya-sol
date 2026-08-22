// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Guard: push into a dynamic-array FIELD of a top-level state-var struct, with
// an eager __postInit (the constructor writes state). Was open as "value too
// long for key 0x7374" — the struct box was created at its default size and a
// grown write was rejected; the write path is del+put now. The mapping-entry
// sibling (m[k].arr.push) has its own guard, test_mapping_struct_dynarray_push.
contract C {
    struct S { uint64[] arr; uint64 x; }
    S st;
    uint256 forcePostInit;
    constructor() { st.x = 7; forcePostInit = 1; }
    function push(uint64 v) public { st.arr.push(v); }
    function pop() public { st.arr.pop(); }
    function len() public view returns (uint256) { return st.arr.length; }
    function get(uint256 i) public view returns (uint64) { return st.arr[i]; }
    function getX() public view returns (uint64) { return st.x; }
}
