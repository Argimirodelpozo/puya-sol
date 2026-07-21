// SPDX-License-Identifier: MIT
pragma solidity ^0.8.25;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the final fable-review-3 asm batch:
//   H12 asm revert(off,len) delivers the payload (log + revert-data stack);
//   M12 calldatacopy zero-pads past calldatasize;
//   M13 mcopy has memmove semantics for overlapping ranges;
//   M7  keccak256/memory reads are slot-routed (offset >= 4096).
contract AsmPayloadMemBatch {
    // H12-a: 4-byte custom-error selector idiom.
    function revSelector() external pure returns (uint256) {
        assembly {
            mstore(0x00, 0x12345678) // right-aligned: bytes 28..32
            revert(0x1c, 4)
        }
    }

    // H12-b: 36-byte payload (selector + one word arg).
    function revWithArg(uint256 v) external pure returns (uint256) {
        assembly {
            mstore(0x00, 0xdeadbeef)
            mstore(0x20, v)
            revert(0x1c, 0x24)
        }
    }

    // H12-c: dynamic length (single-slot path).
    function revDyn(uint256 len) external pure returns (uint256) {
        assembly {
            mstore(0x00, 0x1111222233334444555566667777888899990000aaaabbbbccccddddeeeeffff)
            revert(0, len)
        }
    }

    // H12-e: dynamic-length payload straddling the 4096-byte slot boundary
    // (spliced from slot 0 tail + slot 1 head into one log).
    function revStraddle(uint256 len) external pure returns (uint256) {
        assembly {
            mstore(0x0fe0, 0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa)
            mstore(0x1000, 0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb)
            revert(0x0ff0, len)
        }
    }

    // H12-d: bare revert keeps empty revert data.
    function revBare() external pure returns (uint256) {
        assembly {
            revert(0, 0)
        }
    }

    // M12: copy straddling the calldata end zero-pads. The bytes-calldata
    // param + b.offset read activate the synthetic __cd_blob transport.
    function cdcTail(bytes calldata b) external pure returns (bytes32 r) {
        assembly {
            let q := calldataload(sub(b.offset, 32))
            calldatacopy(0x80, sub(calldatasize(), 8), 32) // 8 real + 24 zeros
            r := mload(0x80)
        }
        b;
    }

    // M13-a: overlapping mcopy, dst inside src (the corrupting direction for
    // a forward word loop).
    function mcopyOverlap() external pure returns (bytes32 a, bytes32 b, bytes32 c) {
        assembly {
            mstore(0x80, 0x0101010101010101010101010101010101010101010101010101010101010101)
            mstore(0xa0, 0x0202020202020202020202020202020202020202020202020202020202020202)
            mcopy(0xa0, 0x80, 64) // dst overlaps src by 32 bytes
            a := mload(0x80)
            b := mload(0xa0)
            c := mload(0xc0)
        }
    }

    // M13-b: overlapping mcopy with a sub-word tail.
    function mcopyOverlapTail() external pure returns (bytes32 a, bytes32 b) {
        assembly {
            mstore(0x80, 0x1111111111111111111111111111111111111111111111111111111111111111)
            mstore(0xa0, 0x2222222222222222222222222222222222222222222222222222222222222222)
            mcopy(0x90, 0x80, 40) // shift by 16, 1 word + 8-byte tail
            a := mload(0x80)
            b := mload(0xa0)
        }
    }

    // M7: keccak over memory beyond the first 4096-byte slot.
    function keccakHigh(uint256 x) external pure returns (bytes32 h1, bytes32 h2) {
        assembly {
            mstore(0x1080, x) // slot 1 in the scratch model
            h1 := keccak256(0x1080, 32)
            mstore(0x1090, x) // straddles a word boundary layout inside slot 1
            h2 := keccak256(0x1084, 44)
        }
    }

    // M7: mload/mstore round-trip at a high offset via the routed helpers.
    function memHighRoundtrip(uint256 x) external pure returns (uint256 r) {
        assembly {
            mstore(0x2000, x) // slot 2
            r := mload(0x2000)
        }
    }
}
