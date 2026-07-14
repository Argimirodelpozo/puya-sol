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
}
