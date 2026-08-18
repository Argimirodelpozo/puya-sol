// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// CUSTOM puya-sol regression — multi-var Yul assignment into a blob-backed
// bytes. `b, tag := mk(m)` must route b's write through its blob offset var
// exactly like the single-var form, or the repoint is invisible and b reads
// as its stale pre-repoint (empty) allocation.
contract MultiAssignBlobRepoint {
    function alloc(bytes memory m)
        public
        pure
        returns (uint256 lenPlusTag, bytes32 firstWord)
    {
        bytes memory b;
        uint256 tag;
        assembly {
            function mk(src) -> ptr, t {
                ptr := mload(0x40)
                mstore(ptr, mload(src))                       // length
                mstore(add(ptr, 0x20), mload(add(src, 0x20))) // first data word
                mstore(0x40, add(ptr, 0x60))
                t := 7
            }
            b, tag := mk(m)
        }
        lenPlusTag = b.length + tag;
        assembly {
            firstWord := mload(add(b, 0x20))
        }
    }
}
