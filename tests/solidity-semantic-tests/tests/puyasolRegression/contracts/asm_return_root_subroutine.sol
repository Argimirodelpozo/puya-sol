// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// A Yul return() that ends a LIBRARY / FREE function body: the synthesized
// Solidity epilogue after it is unreachable. Those bodies are root
// subroutines, not contract methods, so they need their own dead-code pass
// (poseidon-solidity's PoseidonT3.hash is exactly this shape).
library Hasher {
    function hash(uint256[2] memory a) public pure returns (uint256) {
        assembly {
            mstore(0, add(mload(a), 7))
            return(0, 0x20)
        }
    }
}

function freeHash(uint256 x) pure returns (uint256) {
    assembly {
        mstore(0, mul(x, 3))
        return(0, 0x20)
    }
}

contract AsmReturnRoot {
    function viaLibrary(uint256 v) external pure returns (uint256) {
        uint256[2] memory a = [v, uint256(0)];
        return Hasher.hash(a);
    }

    function viaFree(uint256 v) external pure returns (uint256) {
        return freeHash(v);
    }
}
