// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M42: Standalone abi.encodeCall, abi.encodeWithSelector, abi.encodeWithSignature.
 * Tests Gap 2 and Gap 3.
 */

interface ITarget {
    function transfer(address to, uint256 amount) external returns (bool);
    function setValues(uint256 a, uint256 b) external;
}

contract AbiEncode {
    /// abi.encodeCall with two args — returns selector + encoded args
    function testEncodeCall(address to, uint256 amount) external pure returns (bytes memory) {
        return abi.encodeCall(ITarget.transfer, (to, amount));
    }

    /// abi.encodeCall with void return
    function testEncodeCallVoid(uint256 a, uint256 b) external pure returns (bytes memory) {
        return abi.encodeCall(ITarget.setValues, (a, b));
    }

    /// abi.encodeWithSelector with explicit selector
    function testEncodeWithSelector(bytes4 sel, uint256 value) external pure returns (bytes memory) {
        return abi.encodeWithSelector(sel, value);
    }

    /// abi.encodeWithSignature with string signature
    function testEncodeWithSignature(uint256 value) external pure returns (bytes memory) {
        return abi.encodeWithSignature("transfer(address,uint256)", value);
    }

    /// Compare encodeCall output length (should be 4 + 32 + 32 = 68 bytes for two uint256 args)
    function testEncodeCallLength(uint256 a, uint256 b) external pure returns (uint256) {
        bytes memory encoded = abi.encodeCall(ITarget.setValues, (a, b));
        return encoded.length;
    }
}
