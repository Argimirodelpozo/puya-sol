// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract SharingConstructors {
    struct Holder { uint256[] items; uint256 count; }
    struct Outer { Holder child; }
    function constructorAlias() external pure returns (uint256, uint256) {
        uint256[] memory input = new uint256[](1);
        Holder memory holder = Holder(input, 1);
        input[0] = 41;
        return (holder.items[0], holder.count);
    }
    function nestedConstructor() external pure returns (uint256, uint256) {
        uint256[] memory input = new uint256[](1);
        Holder memory holder = Holder(input, 2);
        Outer memory outer = Outer(holder);
        holder.count = 43;
        outer.child.items[0] = 47;
        return (input[0], outer.child.count);
    }
    modifier noArguments {
        uint256[] memory first = new uint256[](1);
        uint256[] memory second = first;
        second = new uint256[](2);
        second[0] = 53;
        require(first[0] == 0 && second.length == 2);
        _;
    }
    function modifierAlias() external pure noArguments returns (uint256) { return 59; }
}
