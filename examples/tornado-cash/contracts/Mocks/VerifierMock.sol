// SPDX-License-Identifier: MIT
pragma solidity ^0.7.0;

import "../Tornado.sol";

/// @dev A mock verifier that always returns true for testing purposes.
contract VerifierMock is IVerifier {
    function verifyProof(bytes memory, uint256[6] memory) external pure override returns (bool) {
        return true;
    }
}
