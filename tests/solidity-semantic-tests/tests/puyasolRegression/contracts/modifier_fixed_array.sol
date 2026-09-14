// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Reduced from PrivacyPool's modifier-bound WithdrawProof. Declaration member
// types are storage-located; memory layout must use solc's located member types.
contract ModifierFixedArray {
    struct Inner { uint256[2] words; }
    struct Proof {
        uint256[2] pA;
        uint256[2][2] pB;
        Inner nested;
    }

    modifier update(Proof memory proof) {
        proof.pA[1] += 10;
        proof.pB[1][0] += 20;
        proof.nested.words[1] += 30;
        _;
        proof.pB[0][1] += 40;
    }

    function alter(Proof memory proof) internal pure update(proof) returns (uint256) {
        // The nested fixed arrays occupy pointer slots, not inline data.
        uint256 value;
        assembly {
            let rows := mload(add(proof, 32))
            value := mload(mload(add(rows, 32)))
        }
        require(value == proof.pB[1][0]);
        return value;
    }

    function run(uint256 seed) external pure returns (
        uint256[2] memory, uint256[2][2] memory, uint256[2] memory, uint256
    ) {
        Proof memory proof = Proof(
            [seed, uint256(2)],
            [[uint256(3), uint256(4)], [uint256(5), uint256(6)]],
            Inner([uint256(7), uint256(8)])
        );
        uint256 value = alter(proof);
        return (proof.pA, proof.pB, proof.nested.words, value);
    }
}
