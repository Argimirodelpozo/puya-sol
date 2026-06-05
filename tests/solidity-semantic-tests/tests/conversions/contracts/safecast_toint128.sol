// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// NOTE: puya-sol CUSTOM regression test — NOT part of the upstream Solidity
// semantic-tests suite (added to guard a puya-sol-specific codegen path).

// Repro of OpenZeppelin/V4 SafeCast.toInt128(int256): narrow with an overflow
// guard. The MIN_INT128 edge (-2^127) must round-trip (downcasted == value), not
// spuriously revert; out-of-range must revert. Guards signed int128(int256)
// narrowing + the `downcasted == value` check at the two's-complement boundary.
contract SafeCastToInt128 {
    function toInt128(int256 value) external pure returns (int128 downcasted) {
        downcasted = int128(value);
        require(downcasted == value, "SafeCast: int128 overflow");
    }
}
