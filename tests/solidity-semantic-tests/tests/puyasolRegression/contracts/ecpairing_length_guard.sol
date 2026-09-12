// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the shared ecPairing whole-pair length check. Any multiple of 192
// bytes, including zero, is supported; partial pairs must fail before the
// pairing operation. Nonzero product correctness has separate coverage.
contract EcPairingLengthGuard {
    function pairWrongLen(bytes memory input) external returns (bool) {
        (bool ok, ) = address(8).staticcall(input);
        return ok;
    }
}
