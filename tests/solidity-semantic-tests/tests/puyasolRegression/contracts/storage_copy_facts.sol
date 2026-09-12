// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract ArrayCopyFacts {
    uint256[][2] private target;
    uint256[][1] private source;

    function copy() external returns (uint256, uint256, uint256, uint256) {
        delete target;
        delete source;
        target[0].push(88);
        target[0].push(99);
        target[1].push(66);
        source[0].push(11);
        target = source;
        uint256 tailLength = target[1].length;
        source[0][0] = 77;
        target[0].push();
        target[1].push();
        return (target[0][0], target[0][1], target[1][0], tailLength);
    }

    function selfCopy() external returns (uint256, uint256) {
        delete source;
        source[0].push(17);
        source = source;
        return (source[0].length, source[0][0]);
    }
}

contract StructCopyFacts {
    struct Entry { uint16 tag; uint256[] values; bytes payload; }
    Entry[1] private target;
    Entry[1] private source;

    function copy() external returns (uint16, uint256, uint256, bytes memory) {
        delete target;
        delete source;
        target[0].values.push(88);
        target[0].values.push(99);
        source[0].tag = 7;
        source[0].values.push(19);
        source[0].payload = hex"010203";
        target = source;
        source[0].values[0] = 77;
        source[0].payload[0] = 0xff;
        target[0].values.push();
        return (target[0].tag, target[0].values[0], target[0].values[1], target[0].payload);
    }
}
