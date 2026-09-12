// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

type Small is int8;
contract ExpressionCalldataFields {
    enum Mode { First, Second }
    struct Item { uint256 n; bool flag; int16 signedValue; bytes3 tag; address owner; Small custom; Mode mode; }
    function repoint(Item calldata a, Item calldata b)
        external pure returns (uint256, bool, int256, bytes3, bool, int256, uint256)
    {
        assembly { a := b }
        return (a.n, a.flag, a.signedValue, a.tag, a.owner == address(0x1234),
                Small.unwrap(a.custom), uint256(a.mode));
    }
    function zeroPadded(Item calldata a, uint256 pointer) external pure returns (uint256, bool) {
        assembly { a := pointer }
        return (a.n, a.flag);
    }
}
