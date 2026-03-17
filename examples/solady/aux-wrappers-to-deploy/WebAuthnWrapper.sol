// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/WebAuthn.sol";

contract WebAuthnWrapper {
    function verify(
        bytes calldata challenge,
        bool requireUV,
        WebAuthn.WebAuthnAuth calldata auth,
        bytes32 x,
        bytes32 y
    ) external view returns (bool) {
        return WebAuthn.verify(challenge, requireUV, auth, x, y);
    }
}
