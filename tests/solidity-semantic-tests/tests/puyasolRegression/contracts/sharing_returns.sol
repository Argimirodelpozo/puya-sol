// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract SharingReturns {
    struct Holder { uint256[] items; }

    function named(uint256[] memory input) internal pure returns (uint256[] memory output) {
        output = input;
    }
    function identity(uint256[] memory input) internal pure returns (uint256[] memory) {
        return input;
    }
    function choose(uint256[] memory input, bool indirect) internal pure returns (uint256[] memory) {
        function(uint256[] memory) internal pure returns (uint256[] memory) target = identity;
        if (indirect) return target(input);
        return input;
    }
    function write(uint256[] memory input) internal pure { input[0] = 23; }

    function namedAlias() external pure returns (uint256) {
        uint256[] memory input = new uint256[](1);
        uint256[] memory output = named(input);
        output[0] = 17;
        return input[0];
    }
    function branchAlias(bool indirect) external pure returns (uint256) {
        uint256[] memory input = new uint256[](1);
        uint256[] memory output = choose(input, indirect);
        output[0] = 19;
        return input[0];
    }
    function returnedArgument() external pure returns (uint256) {
        uint256[] memory input = new uint256[](1);
        write(named(input));
        return input[0];
    }
    function returnedMember() external pure returns (uint256) {
        uint256[] memory input = new uint256[](1);
        Holder memory holder;
        holder.items = named(input);
        input[0] = 29;
        return holder.items[0];
    }
}
