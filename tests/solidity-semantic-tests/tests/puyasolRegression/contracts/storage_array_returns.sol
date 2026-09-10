// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract StorageArrayReturns {
    uint256[] private first;
    uint256[] private second;
    uint16[] private packed;
    uint256[3] private fixedValues;

    function arrays(bool swap) internal view returns (uint256[] storage, uint256[] storage) {
        return swap ? (second, first) : (first, second);
    }

    function words(bool swap) external returns (uint256, uint256, uint256, uint256) {
        delete first;
        delete second;
        first.push(3);
        first.push(5);
        second.push(7);
        second.push(11);
        (uint256[] storage a, uint256[] storage b) = arrays(swap);
        uint256 before = a[1];
        a[0] = 13;
        b[1] = 17;
        return (before, first[0], second[1], b[0]);
    }

    function packedRef() internal view returns (uint16[] storage, uint256) {
        return (packed, 19);
    }

    function packedWords() external returns (uint256, uint256, uint256, uint256, uint256) {
        delete packed;
        for (uint16 i = 0; i < 18; ++i) packed.push(i + 1);
        (uint16[] storage a, uint256 n) = packedRef();
        a[1] = 65535;
        a[16] = 99;
        return (a[0], packed[1], a[15], packed[16], packed[17] + n);
    }

    function fixedRef() internal view returns (uint256[3] storage, uint256) {
        return (fixedValues, 29);
    }

    function fixedWords() external returns (uint256, uint256, uint256) {
        fixedValues = [uint256(3), 5, 7];
        (uint256[3] storage a, uint256 n) = fixedRef();
        a[1] = n;
        return (a[0], fixedValues[1], a[2]);
    }
}
