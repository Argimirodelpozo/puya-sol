pragma solidity ^0.8.24;

contract BoolWordCleanup {
    function clean(uint256 word) external pure returns (bool) {
        bool[] memory values = new bool[](1);
        assembly { mstore(add(values, 32), word) }
        return values[0];
    }

    function validate(uint256 word) external pure returns (bool) {
        return abi.decode(abi.encode(word), (bool));
    }
}
