// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// A public array getter must panic (0x32) on an out-of-range index, exactly
// as `arr[i]` does. Reading past the end silently returned decoded garbage.
contract GetterBounds {
    struct S { uint256 a; string b; uint256 c; }
    S[] public sets;
    uint256[] public nums;
    uint256[3] public fixedNums;
    mapping(uint256 => uint256[]) public nested;

    function seed() external {
        sets.push(S(11, "x", 22));
        nums.push(7);
        nums.push(8);
        nested[5].push(99);
    }
}
