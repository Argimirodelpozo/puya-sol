// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// A push onto a top-level storage array whose box was never eagerly created:
// a deploy that defers the constructor (proxy-runtime replay of an
// upgradeable implementation) never runs __postInit, so the resize must
// materialise the root box itself.
contract DeferredCtorPush {
    struct S { uint256 root; string cid; uint256 ts; }
    S[] internal sets;
    uint256[] internal nums;

    function updateRoot(uint256 r, string memory c) external returns (uint256) {
        sets.push(S(r, c, block.timestamp));
        return sets.length - 1;
    }

    function latestRoot() external view returns (uint256) {
        return sets[sets.length - 1].root;
    }

    function pushNum(uint256 v) external {
        nums.push(v);
    }

    function numsLength() external view returns (uint256) {
        return nums.length;
    }
}
