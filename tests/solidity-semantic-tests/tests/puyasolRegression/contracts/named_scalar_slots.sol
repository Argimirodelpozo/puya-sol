// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

contract NamedScalarSlots {
    uint256 word;
    uint8 small;
    bool flag;
    uint256[] values;

    function scalarSlots() external returns (uint256, uint8, bool, uint256) {
        delete values;
        values.push(33);
        assembly { sstore(word.slot, 7) sstore(small.slot, 0x0109) }
        return (word, small, flag, values[0]);
    }

    function metadata() external pure returns (uint256 slot) {
        assembly { slot := values.slot }
    }
}
