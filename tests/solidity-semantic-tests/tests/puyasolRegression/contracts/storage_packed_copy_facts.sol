// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract PackedCopyFacts {
    uint8[2] private source;
    uint8[4] private target;
    uint160[1] private wideSource;
    uint160[1] private wideTarget;
    struct Pair { uint8 x; uint16 y; }
    Pair[1] private pairsSource;
    Pair[1] private pairsTarget;

    function packed(uint256 word) external returns (uint256 result, uint256, uint256) {
        assembly { sstore(source.slot, word) }
        target = source;
        assembly { result := sload(target.slot) }
        return (result, target[2], target[3]);
    }

    function wide(uint256 word) external returns (uint256 result) {
        assembly { sstore(wideSource.slot, word) }
        wideTarget = wideSource;
        assembly { result := sload(wideTarget.slot) }
    }

    function selfCopy(uint256 word) external returns (uint256 result) {
        assembly { sstore(source.slot, word) }
        source = source;
        assembly { result := sload(source.slot) }
    }

    function structPadding(uint256 word) external returns (uint256 result) {
        assembly { sstore(pairsTarget.slot, word) }
        pairsSource[0] = Pair(1, 2);
        pairsTarget = pairsSource;
        assembly { result := sload(pairsTarget.slot) }
    }
}
