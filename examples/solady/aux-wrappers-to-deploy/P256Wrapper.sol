// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/P256.sol";

contract P256Wrapper {
    function verifySignature(bytes32 hash, bytes32 r, bytes32 s, bytes32 qx, bytes32 qy) external view returns (bool) {
        return P256.verifySignature(hash, r, s, qx, qy);
    }
}
