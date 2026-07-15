// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Guard: inline-assembly `mstore8` into a `bytes memory` array whose length
// exceeds 64 bytes. The base local stays a VALUE (raw bytes); the old generic
// path did `add(m, k)` as a bigint `b+` on that value, which reverts once it is
// larger than 64 bytes (AVM bigint-operand limit). Now a dedicated write handler
// computes the data index and writes one byte via a guarded replace3.
contract C {
    // write 0x42 at data index k of a fresh new bytes(n); return byte@k, byte@0, len
    function poke(uint n, uint k) external pure returns (uint8, uint8, uint) {
        bytes memory m = new bytes(n);
        assembly {
            mstore8(add(add(m, 32), k), 0x42)
        }
        return (uint8(m[k]), uint8(m[0]), m.length);
    }

    // the exact byte_array_to_storage_cleanup shape: write one byte just past the
    // logical end (index == len), which EVM leaves in padding a bounded copy drops.
    // Must be a no-op, not a revert.
    function pokePadding(uint n) external pure returns (uint) {
        bytes memory m = new bytes(n);
        assembly {
            mstore8(add(add(m, 32), n), 0x42)
        }
        return m.length;
    }

    // sibling: full WORD write (mstore) at data offset k — same b+ failure mode,
    // plus the pre-fix handler broke for len > 32 even at offset 0. Observe the
    // word's MSB at k, LSB at k+31, byte 0, and length.
    function pokeWord(uint n, uint k) external pure returns (uint8, uint8, uint8, uint) {
        bytes memory m = new bytes(n);
        uint v = uint(bytes32(hex"AA000000000000000000000000000000000000000000000000000000000000BB"));
        assembly {
            mstore(add(add(m, 32), k), v)
        }
        uint8 last = (k + 31 < m.length) ? uint8(m[k + 31]) : 0;
        return (uint8(m[k]), last, uint8(m[0]), m.length);
    }

    // word write straddling the end (offset = len-1): writes exactly the MSB,
    // drops the 31-byte tail spill (EVM adjacent memory a bounded copy never sees)
    function pokeWordTail(uint n) external pure returns (uint8, uint) {
        bytes memory m = new bytes(n);
        uint v = uint(bytes32(hex"CC00000000000000000000000000000000000000000000000000000000000000"));
        assembly {
            mstore(add(add(m, 32), sub(n, 1)), v)
        }
        return (uint8(m[n - 1]), m.length);
    }

    // legacy offset-0 shape into a SHORT array: value truncated to len (old handler
    // semantics, must not regress)
    function pokeWordShort() external pure returns (uint8, uint8, uint) {
        bytes memory m = new bytes(8);
        uint v = uint(bytes32(hex"1122334455667788990000000000000000000000000000000000000000000000"));
        assembly {
            mstore(add(m, 32), v)
        }
        return (uint8(m[0]), uint8(m[7]), m.length);
    }
}
