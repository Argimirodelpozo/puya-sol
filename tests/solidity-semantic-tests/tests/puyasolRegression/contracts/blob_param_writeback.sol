// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// >4096-byte struct: blob-backed memory (pointer model). uint256[129] = 4128 B.
struct Big { uint256[129] a; }

library L {
    // Mutating library callee with a blob param: writes go through the
    // shared blob; the return must NOT be augmented with the param.
    function put(Big memory b, uint256 v) internal {
        b.a[3] = v;
    }

    // Compound inside the library callee (blob leaf read-modify-write).
    function bump(Big memory b) internal {
        b.a[2] += 1;
    }
}

contract BlobParamWriteback {
    function libWrite(uint256 v) external returns (uint256) {
        Big memory big;
        L.put(big, v);
        return big.a[3];   // callee wrote through the blob
    }

    function compoundLeaf(uint256 v) external returns (uint256) {
        Big memory big;
        big.a[7] = v;
        big.a[7] += 5;
        return big.a[7];
    }

    function libCompound(uint256 v) external returns (uint256) {
        Big memory big;
        big.a[2] = v;
        L.bump(big);
        return big.a[2];
    }
}
