// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

import "abstract/SelfVerificationRoot.sol";
import "libraries/CircuitAttributeHandlerV2.sol";

/**
 * M13: Tests SelfVerificationRoot abstract contract integration.
 * A minimal concrete subclass that exposes testable internal functions
 * without requiring an actual hub or Poseidon deployment.
 */
contract SelfVerificationRootTest is SelfVerificationRoot {
    // Track verification calls for testing
    bytes32 public lastAttestationId;
    uint256 public lastOlderThan;

    constructor(address hubAddress)
        SelfVerificationRoot(hubAddress, "test-scope")
    {}

    // Override getConfigId (required by base)
    function getConfigId(
        bytes32 destinationChainId,
        bytes32 userIdentifier,
        bytes memory /* userDefinedData */
    ) public view override returns (bytes32) {
        return keccak256(abi.encodePacked(destinationChainId, userIdentifier));
    }

    // Override customVerificationHook to record output
    function customVerificationHook(
        ISelfVerificationRoot.GenericDiscloseOutputV2 memory output,
        bytes memory /* userData */
    ) internal override {
        lastAttestationId = output.attestationId;
        lastOlderThan = output.olderThan;
    }

    // === Test functions that exercise SelfVerificationRoot internals ===

    // Test scope() returns 0 on localnet (no Poseidon)
    function testScope() external view returns (uint256) {
        return scope();
    }

    // Test building the baseVerificationInput (abi.encodePacked pattern)
    function testBuildBaseInput(bytes memory proofPayload) external view returns (bytes memory) {
        return abi.encodePacked(
            uint8(2),       // CONTRACT_VERSION
            bytes31(0),     // 31 bytes buffer
            _scope,         // 32 bytes scope
            proofPayload    // proof data
        );
    }

    // Test bytes.concat
    function testBytesConcat(bytes memory a, bytes memory b) external pure returns (bytes memory) {
        return bytes.concat(a, b);
    }

    // Test keccak256-based configId generation
    function testConfigId(bytes32 chainId, bytes32 userId) external pure returns (bytes32) {
        return keccak256(abi.encodePacked(chainId, userId));
    }

    // Test cross-library: getOlderThan through CircuitAttributeHandlerV2
    function testPassportOlderThan(bytes memory charcodes) external pure returns (uint256) {
        return CircuitAttributeHandlerV2.getOlderThan(AttestationId.E_PASSPORT, charcodes);
    }

    // Test cross-library: extractStringAttribute
    function testExtractString(bytes memory charcodes, uint256 start, uint256 end) external pure returns (string memory) {
        return CircuitAttributeHandlerV2.extractStringAttribute(charcodes, start, end);
    }

    // Test SelfUtils.stringToBigInt integration
    function testStringToBigInt(string memory s) external pure returns (uint256) {
        return SelfUtils.stringToBigInt(s);
    }

    // Test Formatter.substring
    function testSubstring(string memory s, uint256 start, uint256 end) external pure returns (string memory) {
        return Formatter.substring(s, start, end);
    }
}
