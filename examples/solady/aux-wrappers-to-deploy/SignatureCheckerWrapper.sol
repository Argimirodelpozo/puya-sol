// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/SignatureCheckerLib.sol";

contract SignatureCheckerWrapper {
    function isValidSignatureNow(address signer, bytes32 hash, bytes calldata signature) external view returns (bool) {
        return SignatureCheckerLib.isValidSignatureNow(signer, hash, signature);
    }
}
