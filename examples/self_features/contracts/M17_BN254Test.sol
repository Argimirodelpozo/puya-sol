// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0 <0.9.0;

/// Minimal BN254 test contract for ec_add and ec_scalar_mul.
contract BN254Test {
    uint256 constant q = 21888242871839275222246405745257275088696311157297823662689037894645226208583;

    /// ecAddX: add two G1 points, return result x-coordinate only.
    function ecAddX(
        uint256 ax, uint256 ay,
        uint256 bx, uint256 by
    ) external view returns (uint256) {
        uint256 rx;
        assembly {
            let mIn := mload(0x40)
            mstore(mIn, ax)
            mstore(add(mIn, 32), ay)
            mstore(add(mIn, 64), bx)
            mstore(add(mIn, 96), by)
            let success := staticcall(sub(gas(), 2000), 6, mIn, 128, mIn, 64)
            if iszero(success) {
                mstore(0, 0)
                return(0, 0x20)
            }
            rx := mload(mIn)
        }
        return rx;
    }

    /// ecMulX: scalar multiply a G1 point, return result x-coordinate.
    function ecMulX(
        uint256 px, uint256 py, uint256 s
    ) external view returns (uint256) {
        uint256 rx;
        assembly {
            let mIn := mload(0x40)
            mstore(mIn, px)
            mstore(add(mIn, 32), py)
            mstore(add(mIn, 64), s)
            let success := staticcall(sub(gas(), 2000), 7, mIn, 96, mIn, 64)
            if iszero(success) {
                mstore(0, 0)
                return(0, 0x20)
            }
            rx := mload(mIn)
        }
        return rx;
    }

    /// Negate a G1 point's y-coordinate.
    function negateY(uint256 y) external pure returns (uint256) {
        uint256 result;
        assembly {
            result := mod(sub(q, y), q)
        }
        return result;
    }
}
