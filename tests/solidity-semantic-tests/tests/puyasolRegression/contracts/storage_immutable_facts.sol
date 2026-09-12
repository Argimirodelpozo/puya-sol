// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract ImmutableLeft {
    uint256 private immutable value;
    constructor() { value = 11; }
    function left() public view returns (uint256) { return value; }
}
contract ImmutableRight {
    uint256 private immutable value;
    constructor() { value = 22; }
    function right() public view returns (uint256) { return value; }
}
contract ImmutableLR is ImmutableLeft, ImmutableRight {
    function both() external view returns (uint256, uint256) { return (left(), right()); }
}
contract ImmutableRL is ImmutableRight, ImmutableLeft {
    function both() external view returns (uint256, uint256) { return (left(), right()); }
}
