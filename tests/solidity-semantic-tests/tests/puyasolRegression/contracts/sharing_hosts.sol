// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract SharingBase {
    function change(uint256[] memory value) internal pure virtual { value[0] = 31; }
    function relay(uint256[] memory value) internal pure { change(value); }
    function check() external pure virtual returns (uint256) {
        uint256[] memory baseValue = new uint256[](1);
        relay(baseValue);
        return baseValue[0];
    }
}

contract SharingDerived is SharingBase {
    function change(uint256[] memory value) internal pure override {
        uint256[] memory aliasValue = value;
        aliasValue[0] = 37;
    }
    function check() external pure override returns (uint256) {
        uint256[] memory derivedValue = new uint256[](1);
        relay(derivedValue);
        return derivedValue[0];
    }
}
