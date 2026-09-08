// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

interface IProofGateway {
    function verifyProof(bytes32 programVKey, bytes calldata publicValues, bytes calldata proof) external view;
    function routes(bytes4 selector) external view returns (address verifier, bool frozen);
}

/// Synthetic stateful consumer, not a historical deployed application.
/// The gateway and verifier used by the test are the verified SP1 contracts.
contract ProofApplication {
    IProofGateway public immutable gateway;
    bytes32 public immutable programVKey;
    uint256 private accepted;
    bytes32 private lastDigest;

    constructor(address gateway_, bytes32 programVKey_) {
        gateway = IProofGateway(gateway_);
        programVKey = programVKey_;
    }

    function submitProof(bytes calldata publicValues, bytes calldata proof) external returns (bytes32 digest) {
        digest = sha256(publicValues);
        // Intentionally write before the call: rejection in either callee must
        // roll these writes back with the entire outer transaction.
        ++accepted;
        lastDigest = digest;
        gateway.verifyProof(programVKey, publicValues, proof);
    }

    function state() external view returns (uint256, bytes32) {
        return (accepted, lastDigest);
    }

    function routeStatus(bytes4 selector) external view returns (address, bool) {
        return gateway.routes(selector);
    }
}
