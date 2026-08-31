// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Coverage lane for assembly/PrecompileHandlers.cpp: every implemented EVM
// precompile invoked through the RAW asm staticcall shape.
//   0x1 ecRecover  0x2 SHA-256  0x4 identity  0x5 modExp
//   0x6 ecAdd      0x7 ecMul    0x8 ecPairing
// ecrecover vector = upstream's known-good fixture (ecrecover.sol).
contract PrecompilesAsm {
    // ── 0x2 SHA-256 ─────────────────────────────────────────────────────
    function sha(bytes memory data) public view returns (bytes32 out) {
        assembly {
            let ok := staticcall(gas(), 2, add(data, 32), mload(data), 0, 32)
            if iszero(ok) { revert(0, 0) }
            out := mload(0)
        }
    }

    // ── 0x4 identity (memcpy) ───────────────────────────────────────────
    function idcopy(bytes32 word) public view returns (bytes32 out) {
        assembly {
            mstore(64, word)
            let ok := staticcall(gas(), 4, 64, 32, 96, 32)
            if iszero(ok) { revert(0, 0) }
            out := mload(96)
        }
    }

    // ── 0x5 modExp (32-byte operands) ───────────────────────────────────
    function modexp(uint256 b, uint256 e, uint256 m)
        public view returns (uint256 out)
    {
        assembly {
            let p := 128
            mstore(p, 32)             // base length
            mstore(add(p, 32), 32)    // exponent length
            mstore(add(p, 64), 32)    // modulus length
            mstore(add(p, 96), b)
            mstore(add(p, 128), e)
            mstore(add(p, 160), m)
            let ok := staticcall(gas(), 5, p, 192, p, 32)
            if iszero(ok) { revert(0, 0) }
            out := mload(p)
        }
    }

    // ── 0x1 ecRecover ───────────────────────────────────────────────────
    function recover(bytes32 h, uint256 v, bytes32 r, bytes32 s)
        public view returns (address out)
    {
        assembly {
            let p := 128
            mstore(p, h)
            mstore(add(p, 32), v)
            mstore(add(p, 64), r)
            mstore(add(p, 96), s)
            let ok := staticcall(gas(), 1, p, 128, p, 32)
            if iszero(ok) { revert(0, 0) }
            out := mload(p)
        }
    }

    // ── 0x6 ecAdd: P + (-P) = the point at infinity (0, 0) ─────────────
    function ecAddInverse() public view returns (uint256 x, uint256 y) {
        // BN254 generator (1, 2) and its negation (1, p - 2).
        uint256 negY =
            21888242871839275222246405745257275088696311157297823662689037894645226208581;
        assembly {
            let p := 128
            mstore(p, 1)
            mstore(add(p, 32), 2)
            mstore(add(p, 64), 1)
            mstore(add(p, 96), negY)
            let ok := staticcall(gas(), 6, p, 128, p, 64)
            if iszero(ok) { revert(0, 0) }
            x := mload(p)
            y := mload(add(p, 32))
        }
    }

    // ── 0x7 ecMul: G * 1 = G ────────────────────────────────────────────
    function ecMulOne() public view returns (uint256 x, uint256 y) {
        assembly {
            let p := 128
            mstore(p, 1)
            mstore(add(p, 32), 2)
            mstore(add(p, 64), 1)
            let ok := staticcall(gas(), 7, p, 96, p, 64)
            if iszero(ok) { revert(0, 0) }
            x := mload(p)
            y := mload(add(p, 32))
        }
    }

    // ── 0x8 ecPairing: the empty product is 1 (true) ────────────────────
    function pairEmpty() public view returns (uint256 out) {
        assembly {
            let ok := staticcall(gas(), 8, 0, 0, 0, 32)
            if iszero(ok) { revert(0, 0) }
            out := mload(0)
        }
    }
}
