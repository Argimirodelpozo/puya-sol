// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the ecPairing input-length assert (fable-review-3 M9). The AVM
// reshaping hard-codes the 2-pair (384-byte) layout; a longer input (Groth16
// verifiers use 3-4 pairs) previously checked only pairs 0-1 — accepting
// invalid proofs — and a shorter one panicked mid-extract. Now anything but
// exactly 384 bytes reverts loudly at the length check (before any pairing op).
contract EcPairingLengthGuard {
    function pairWrongLen(bytes memory input) external returns (bool) {
        (bool ok, ) = address(8).staticcall(input);
        return ok;
    }
}
