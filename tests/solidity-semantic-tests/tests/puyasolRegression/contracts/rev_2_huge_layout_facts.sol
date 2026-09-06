// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract HugeLayoutFacts {
    uint8 first = 7;
    uint8[2**200] enormous;
    uint32 last = 9;

    function facts() external view returns (
        uint256 length, uint256 firstSlot, uint256 arraySlot,
        uint256 lastSlot, uint256 firstValue, uint256 lastValue
    ) {
        length = enormous.length;
        assembly {
            firstSlot := first.slot
            arraySlot := enormous.slot
            lastSlot := last.slot
        }
        firstValue = first;
        lastValue = last;
    }

    function change() external { first = 11; last = 13; }
}
