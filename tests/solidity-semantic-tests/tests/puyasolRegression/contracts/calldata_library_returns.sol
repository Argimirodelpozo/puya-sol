// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

library CalldataLibrary {
    function identity(bytes calldata x) public pure returns (bytes calldata) { return x; }
    function pair(bytes calldata x, bytes calldata y) public pure returns (bytes calldata, bytes calldata) {
        return (x, y);
    }
    function touch(bytes memory x, bytes calldata y) public pure returns (bytes calldata) {
        x[0] = 0xff;
        return y;
    }
    function coordinates(bytes calldata x) public pure returns (uint256 off, uint256 len) {
        assembly { off := x.offset len := x.length }
    }
    function selector(bytes calldata) public pure returns (bytes4 s) { assembly { s := calldataload(0) } }
    function message(bytes calldata) public pure returns (bytes calldata) { return msg.data; }
}

contract LibraryCalldataReturns {
    using CalldataLibrary for bytes;
    function fromMemory() external pure returns (bytes1 original, bytes memory result) {
        bytes memory x = hex"6162";
        result = CalldataLibrary.identity(x);
        result[0] = 0xff;
        return (x[0], result);
    }
    function fromCalldata(bytes calldata x) external pure returns (bytes memory) { return CalldataLibrary.identity(x); }
    function usingFor(bytes calldata x) external pure returns (bytes memory) { return x.identity(); }
    function tupleCopy(bytes calldata x, bytes calldata y) external pure returns (bytes memory, bytes memory) {
        return CalldataLibrary.pair(x, y);
    }
    function immutableInput(bytes calldata x) external pure returns (bytes1, bytes memory) {
        bytes memory original = x;
        bytes memory result = CalldataLibrary.touch(original, original);
        return (original[0], result);
    }
    function coordinates(bytes calldata x) external pure returns (uint256 off, uint256 len, uint256 original) {
        (off, len) = CalldataLibrary.coordinates(x);
        assembly { original := x.offset }
    }
    function selector(bytes calldata x) external pure returns (bool) {
        return CalldataLibrary.selector(x) == CalldataLibrary.selector.selector;
    }
    function libraryMessage(bytes calldata x) external pure returns (bool) {
        return keccak256(CalldataLibrary.message(x)) == keccak256(abi.encodeWithSelector(CalldataLibrary.message.selector, x));
    }
}
