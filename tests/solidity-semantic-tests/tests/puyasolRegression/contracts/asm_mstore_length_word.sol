// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). Guards the mstore LENGTH-WORD write.
//
// `mstore(ptr, n)` where ptr is a bytes/string buffer POINTER (not ptr+32)
// targets the EVM length word — it resizes the buffer in place. Only the
// `add(ptr, k)` data-pointer form was matched, so a bare pointer fell through
// to the generic assembly path and hard-errored:
//     cannot coerce non-scalar type 'string' to biguint in assembly arithmetic
//
// This is the OZ ShortStrings.toString idiom, and it blocked three real
// deployed contracts (kaito / degen / builder) in the chainwide replay sweep.
//
// In puya-sol's value model the local IS the raw bytes with no length header,
// so the length write lowers to a resize: truncate, or zero-extend when
// growing. Both orderings (length-then-data, data-then-length) must agree,
// since the data write is itself clamped to the buffer length.
contract AsmMstoreLengthWord {
    // length write BEFORE the data write (the OZ ShortStrings order)
    function toStr(bytes32 sstr, uint256 len) external pure returns (string memory) {
        require(len <= 32);
        string memory str = new string(32);
        assembly {
            mstore(str, len)
            mstore(add(str, 0x20), sstr)
        }
        return str;
    }

    // length write AFTER the data write
    function toStrRev(bytes32 sstr, uint256 len) external pure returns (string memory) {
        require(len <= 32);
        string memory str = new string(32);
        assembly {
            mstore(add(str, 0x20), sstr)
            mstore(str, len)
        }
        return str;
    }

    // bytes (not string), truncation
    function shrink(bytes32 w, uint256 len) external pure returns (bytes memory) {
        require(len <= 32);
        bytes memory b = new bytes(32);
        assembly {
            mstore(add(b, 0x20), w)
            mstore(b, len)
        }
        return b;
    }

    // GROW past the allocation — zero-extends on AVM (EVM would expose adjacent
    // memory, which has no AVM analogue; only the length is observed here).
    function growLen(uint256 len) external pure returns (uint256) {
        require(len <= 64);
        bytes memory b = new bytes(8);
        assembly { mstore(b, len) }
        return b.length;
    }
}
