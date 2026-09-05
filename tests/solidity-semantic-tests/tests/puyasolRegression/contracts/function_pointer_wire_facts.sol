// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

type PointerUnsigned is uint128;
type PointerSigned is int128;

contract FunctionPointerWireFacts {
    function plusOne(PointerUnsigned value) public pure returns (PointerUnsigned) {
        return PointerUnsigned.wrap(PointerUnsigned.unwrap(value) + 1);
    }
    function plusTwo(PointerUnsigned value) public pure returns (PointerUnsigned) {
        return PointerUnsigned.wrap(PointerUnsigned.unwrap(value) + 2);
    }
    function signedOne(PointerSigned value) public pure returns (PointerSigned) {
        return PointerSigned.wrap(PointerSigned.unwrap(value) + 1);
    }
    function signedTwo(PointerSigned value) public pure returns (PointerSigned) {
        return PointerSigned.wrap(PointerSigned.unwrap(value) + 2);
    }
    function dynamicUnsigned(bool first, uint128 value) external pure returns (uint128) {
        function(PointerUnsigned) pure returns (PointerUnsigned) pointer = first ? plusOne : plusTwo;
        return PointerUnsigned.unwrap(pointer(PointerUnsigned.wrap(value)));
    }
    function staticUnsigned(uint128 value) external pure returns (uint128) {
        function(PointerUnsigned) pure returns (PointerUnsigned) pointer = plusOne;
        return PointerUnsigned.unwrap(pointer(PointerUnsigned.wrap(value)));
    }
    function dynamicSigned(bool first, int128 value) external pure returns (int128) {
        function(PointerSigned) pure returns (PointerSigned) pointer = first ? signedOne : signedTwo;
        return PointerSigned.unwrap(pointer(PointerSigned.wrap(value)));
    }
    function staticSigned(int128 value) external pure returns (int128) {
        function(PointerSigned) pure returns (PointerSigned) pointer = signedOne;
        return PointerSigned.unwrap(pointer(PointerSigned.wrap(value)));
    }
}
