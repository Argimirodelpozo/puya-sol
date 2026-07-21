// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards ternary-INIT storage pointers writing THROUGH to the selected root
// (formerly a documented known-gap: mutations went into a materialized value
// copy). The pointer binds a runtime-selected box key at declaration time.
contract TernaryStoragePtrMut {
    uint256[] a1;
    uint256[] a2;

    function pushThrough(bool c, uint256 v)
        external returns (uint256 l1, uint256 l2, uint256 tail)
    {
        delete a1;
        delete a2;
        uint256[] storage p = c ? a1 : a2;
        p.push(v);
        p.push(v + 1);
        l1 = a1.length;
        l2 = a2.length;
        tail = c ? a1[1] : a2[1];
    }

    function writeThrough(bool c, uint256 v) external returns (uint256 r1, uint256 r2) {
        delete a1;
        delete a2;
        a1.push(0);
        a2.push(0);
        uint256[] storage p = c ? a1 : a2;
        p[0] = v;
        r1 = a1[0];
        r2 = a2[0];
    }

    function readThrough(bool c) external returns (uint256) {
        delete a1;
        delete a2;
        a1.push(11);
        a2.push(22);
        uint256[] storage p = c ? a1 : a2;
        return p[0] * 100 + p.length;
    }

    // The selection must be pinned at declaration: mutating the condition's
    // input afterwards must not re-select the root.
    function selectThenFlipCond(uint256 v) external returns (uint256 l1, uint256 l2) {
        delete a1;
        delete a2;
        bool c = true;
        uint256[] storage p = c ? a1 : a2;
        c = false;
        c;
        p.push(v);
        l1 = a1.length;
        l2 = a2.length;
    }
}
