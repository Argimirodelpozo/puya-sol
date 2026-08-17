// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// CUSTOM puya-sol regression fixture — EVM bytecode introspection is a hard
// compile error: the deployed program is TEAL, so any answer (including solc's
// real object) describes a contract that does not exist on chain. The folded
// consumers (.length, keccak256) must not bypass the error.
contract BytecodeSubject {
    function ping(uint256 x) external pure returns (uint256) {
        return x + 7;
    }
}

contract BytecodeObserver {
    function creationLength() external pure returns (uint256) {
        return type(BytecodeSubject).creationCode.length;
    }

    function runtimeHash() external pure returns (bytes32) {
        return keccak256(type(BytecodeSubject).runtimeCode);
    }
}
