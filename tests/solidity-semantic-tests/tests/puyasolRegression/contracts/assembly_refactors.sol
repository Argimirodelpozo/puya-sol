// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract AssemblyRefactors {
    function unsignedSigns(uint64 x) external pure returns (uint256 a, uint256 b, uint256 c) {
        assembly { a := slt(x, 0) b := slt(0, x) c := sgt(x, 0) }
    }
    function signedSigns(int64 x) external pure returns (uint256 a, uint256 b, uint256 c) {
        assembly { a := slt(x, 0) b := slt(0, x) c := sgt(x, 0) }
    }
    function ordered(uint256 x) external pure returns (uint256 a, uint256 b) {
        assembly {
            function change() -> r { mstore(128, 9) r := 1 }
            mstore(128, x)
            a := add(change(), mload(128))
            mstore(128, x)
            b := add(mload(128), change())
        }
    }
    function statementOrder(uint256 x) external pure returns (uint256 r) {
        assembly {
            function destination() -> p { mstore(128, 9) p := 160 }
            mstore(128, x)
            mstore(destination(), mload(128))
            r := mload(160)
        }
    }
    function addressSpaces(uint256 x) external pure returns (uint256 a, uint256 b) {
        assembly { mstore(4, 7) a := mload(4) b := calldataload(4) }
    }
    function fullWidth() external pure returns (uint256 r) {
        assembly { mstore(128, 5) r := mload(add(not(0), 129)) }
    }
    function farCalldata(uint256 off) external pure returns (uint256 r) {
        assembly { r := calldataload(off) }
    }
    function emptyCopy(uint256 off) external pure returns (uint256 r) {
        assembly {
            mcopy(off, off, 0)
            calldatacopy(off, off, 0)
            r := 1
        }
    }
    function wordAt(uint256 off) external pure returns (uint256 r) {
        assembly { mstore(off, 123) r := mload(off) }
    }
    function partialCopy(uint256 off, uint256 size) external pure returns (uint256 r) {
        assembly {
            mstore(128, not(0))
            mstore(160, not(0))
            mstore(off, 0)
            mstore(add(off, 32), 0)
            mcopy(off, 128, size)
            r := mload(off)
        }
    }
}
