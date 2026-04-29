// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/ECDSA.sol";

contract ECDSAWrapper {
    function recover(bytes32 hash, bytes calldata signature) external view returns (address) {
        return ECDSA.recover(hash, signature);
    }

    function recoverCalldata(bytes32 hash, bytes calldata signature) external view returns (address) {
        return ECDSA.recoverCalldata(hash, signature);
    }

    function toEthSignedMessageHash(bytes32 hash) external pure returns (bytes32) {
        return ECDSA.toEthSignedMessageHash(hash);
    }
}
