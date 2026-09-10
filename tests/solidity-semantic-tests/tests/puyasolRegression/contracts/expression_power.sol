// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

contract ExpressionPower {
    function power(uint8 x, uint256 exponent, bool wrap) external pure returns (uint256) {
        if (wrap) { unchecked { return x ** exponent; } }
        return x ** exponent;
    }
    function signedPower(int8 x, uint256 exponent, bool wrap) external pure returns (int256) {
        if (wrap) { unchecked { return x ** exponent; } }
        return x ** exponent;
    }
}
