// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Faithful repro of V4 ProtocolFeeLibrary.calculateSwapFee — a Yul assembly
// block doing  self + lpFee - (self*lpFee / 1_000_000)  with masked inputs.
// self<=0xfff (4095), lpFee<=0xffffff (16777215) so numerator<=~6.9e10 (fits
// uint64) and the result is always >= 0; guards against a uint64 codegen
// over/underflow in mul/div/sub.
contract ProtocolFeeSwapFee {
    uint256 constant PIPS_DENOMINATOR = 1_000_000;

    function calculateSwapFee(uint16 self, uint24 lpFee) external pure returns (uint24 swapFee) {
        assembly ("memory-safe") {
            self := and(self, 0xfff)
            lpFee := and(lpFee, 0xffffff)
            let numerator := mul(self, lpFee)
            swapFee := sub(add(self, lpFee), div(numerator, PIPS_DENOMINATOR))
        }
    }
}
