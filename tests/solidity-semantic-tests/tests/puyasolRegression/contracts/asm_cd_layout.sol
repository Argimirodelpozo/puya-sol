// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the solc-derived EVM-ABI synthetic-calldata layout (possible_solc
// item 2): signed sub-word params sign-extend in their head word; static
// aggregates inline in the head (shifting later params); sub-word-element
// dynamic arrays re-encode to padded 32-byte words; bytes4 left-aligns.
contract AsmCdLayout {
    function f1(int8 s, uint256 x) external pure returns (bytes32 w0, uint256 w1) {
        assembly {
            w0 := calldataload(4)
            w1 := calldataload(0x24)
        }
        s; x;
    }

    function f2(uint8[3] calldata a, uint256 x) external pure returns (uint256 w0, uint256 w2, uint256 xw) {
        assembly {
            w0 := calldataload(4)
            w2 := calldataload(0x44)
            xw := calldataload(0x64)
        }
        a; x;
    }

    function f3(uint8[] calldata a) external pure returns (uint256 n, uint256 e0, uint256 e1, uint256 cds) {
        assembly {
            n := a.length
            e0 := calldataload(a.offset)
            e1 := calldataload(add(a.offset, 32))
            cds := calldatasize()
        }
    }

    function f4(bytes4 b, uint256 x) external pure returns (bytes32 w0, uint256 xw) {
        assembly {
            w0 := calldataload(4)
            xw := calldataload(0x24)
        }
        b; x;
    }

    function f5(int16[] calldata a) external pure returns (bytes32 e0) {
        assembly {
            e0 := calldataload(a.offset)
        }
    }
}
