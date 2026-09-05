// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract PointerBase {
    function baseValue(uint64 x) public pure virtual returns (uint64) { return x + 3; }
    function alternate(uint64 x) public pure returns (uint64) { return x + 5; }

    function run(bool useBase, uint64 x) external pure returns (uint64) {
        function(uint64) pure returns (uint64) target = useBase ? baseValue : alternate;
        return target(x);
    }
}

contract PointerFirst is PointerBase {}

contract PointerSecond is PointerBase {
    function baseValue(uint64 x) public pure override returns (uint64) { return x + 9; }
}
