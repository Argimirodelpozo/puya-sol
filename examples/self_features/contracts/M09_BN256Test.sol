// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

contract BN256Test {
    /// Elliptic curve point addition on BN256 (alt_bn128).
    /// Maps to EVM precompile at address 6.
    function ecAdd(
        uint256 x1, uint256 y1,
        uint256 x2, uint256 y2
    ) external view returns (uint256 rx, uint256 ry) {
        assembly {
            // Store inputs at memory offsets 0x00..0x60
            mstore(0x00, x1)
            mstore(0x20, y1)
            mstore(0x40, x2)
            mstore(0x60, y2)
            // Call ecAdd precompile
            let success := staticcall(gas(), 6, 0x00, 0x80, 0x80, 0x40)
            // Read results
            rx := mload(0x80)
            ry := mload(0xa0)
        }
    }

    /// Elliptic curve scalar multiplication on BN256.
    /// Maps to EVM precompile at address 7.
    function ecMul(
        uint256 x, uint256 y,
        uint256 s
    ) external view returns (uint256 rx, uint256 ry) {
        assembly {
            mstore(0x00, x)
            mstore(0x20, y)
            mstore(0x40, s)
            let success := staticcall(gas(), 7, 0x00, 0x60, 0x60, 0x40)
            rx := mload(0x60)
            ry := mload(0x80)
        }
    }

    /// BN256 pairing check. Returns true if the pairing equation holds.
    /// Maps to EVM precompile at address 8.
    /// Takes 2 pairs: (G1_1, G2_1, G1_2, G2_2)
    /// G1 points are 2 uint256, G2 points are 4 uint256.
    /// So 2 pairs = 2 * (2 + 4) = 12 uint256 values.
    function pairingCheck2(
        uint256 ax1, uint256 ay1,
        uint256 bx1_1, uint256 bx1_2, uint256 by1_1, uint256 by1_2,
        uint256 ax2, uint256 ay2,
        uint256 bx2_1, uint256 bx2_2, uint256 by2_1, uint256 by2_2
    ) external view returns (bool) {
        uint256 result;
        assembly {
            mstore(0x00, ax1)
            mstore(0x20, ay1)
            mstore(0x40, bx1_1)
            mstore(0x60, bx1_2)
            mstore(0x80, by1_1)
            mstore(0xa0, by1_2)
            mstore(0xc0, ax2)
            mstore(0xe0, ay2)
            mstore(0x100, bx2_1)
            mstore(0x120, bx2_2)
            mstore(0x140, by2_1)
            mstore(0x160, by2_2)
            let success := staticcall(gas(), 8, 0x00, 0x180, 0x180, 0x20)
            result := mload(0x180)
        }
        return result == 1;
    }
}
