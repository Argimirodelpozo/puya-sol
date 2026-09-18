// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Functional twin — runs in BOTH storage modes (no asm slot asserts).
contract StorageParityCore {
    struct S {
        uint256 x;
        mapping(uint256 => uint256) mm;
    }
    S internal st;
    uint256[] internal arr;
    uint256[] internal arr2;

    function deleteStructKeepsMapping() public returns (bool ok) {
        st.x = 5;
        st.mm[1] = 6;
        delete st;
        // solc: delete zeroes value members, mappings are UNTOUCHED
        ok = (st.x == 0) && (st.mm[1] == 6);
    }

    function deleteArrayZeroesElements() public returns (bool ok) {
        arr.push(11);
        arr.push(22);
        delete arr;
        // reading past length reverts; re-push and check the slot was zeroed
        arr.push(0);
        ok = (arr.length == 1) && (arr[0] == 0);
    }

    function pushRefAndDefault() public returns (bool ok) {
        while (arr2.length > 0) arr2.pop();
        arr2.push() = 7;      // push() returns a reference
        arr2.push();          // bare push appends zero
        ok = (arr2.length == 2) && (arr2[0] == 7) && (arr2[1] == 0);
    }

    function popThenPushReadsZero() public returns (bool ok) {
        while (arr2.length > 0) arr2.pop();
        arr2.push(99);
        arr2.pop();
        arr2.push();          // must observe a ZEROED slot, not stale 99
        ok = (arr2[0] == 0);
    }

    struct P {
        uint64 u;
        uint256 v;
    }
    P internal p1;
    P internal p2;
    uint256[] internal src;
    uint256[] internal dst;

    function structStorageCopy() public returns (bool ok) {
        p1.u = 3;
        p1.v = 4;
        p2 = p1;              // storage->storage: element-wise DEEP copy
        p1.u = 9;             // independence after the copy
        ok = (p2.u == 3) && (p2.v == 4) && (p1.u == 9);
    }

    function arrayStorageCopy() public returns (bool ok) {
        while (src.length > 0) src.pop();
        while (dst.length > 0) dst.pop();
        src.push(1);
        src.push(2);
        dst.push(77);
        dst.push(88);
        dst.push(99);
        dst = src;            // resizes down AND copies element-wise
        src.push(3);
        ok = (dst.length == 2) && (dst[0] == 1) && (dst[1] == 2)
            && (src.length == 3);
    }
}
