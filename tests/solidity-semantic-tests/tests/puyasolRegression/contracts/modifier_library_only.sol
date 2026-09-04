// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

library OnlyModifierLibrary {
    modifier nonzero(uint256 value) {
        require(value != 0);
        _;
    }

    function bump(uint256 value)
        public
        pure
        nonzero(value)
        returns (uint256)
    {
        return value + 1;
    }
}
