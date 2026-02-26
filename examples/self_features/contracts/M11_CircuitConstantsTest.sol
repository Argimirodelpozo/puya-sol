// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

import "constants/AttestationId.sol";
import "constants/CircuitConstantsV2.sol";
import "libraries/SelfUtils.sol";

/**
 * M11: Tests CircuitConstantsV2 getDiscloseIndices (struct creation/return) and SelfUtils.stringToBigInt.
 * Exercises: struct creation with named fields, bytes32 constants, bytes32 equality,
 * struct field access, string-to-bytes conversion, left shift, bitwise OR.
 */
contract CircuitConstantsTest {
    // Test getDiscloseIndices for E_PASSPORT — return a specific field
    function testPassportNullifierIndex() external pure returns (uint256) {
        CircuitConstantsV2.DiscloseIndices memory indices = CircuitConstantsV2.getDiscloseIndices(AttestationId.E_PASSPORT);
        return indices.nullifierIndex;
    }

    function testPassportScopeIndex() external pure returns (uint256) {
        CircuitConstantsV2.DiscloseIndices memory indices = CircuitConstantsV2.getDiscloseIndices(AttestationId.E_PASSPORT);
        return indices.scopeIndex;
    }

    function testEuIdNullifierIndex() external pure returns (uint256) {
        CircuitConstantsV2.DiscloseIndices memory indices = CircuitConstantsV2.getDiscloseIndices(AttestationId.EU_ID_CARD);
        return indices.nullifierIndex;
    }

    function testAadhaarNullifierIndex() external pure returns (uint256) {
        CircuitConstantsV2.DiscloseIndices memory indices = CircuitConstantsV2.getDiscloseIndices(AttestationId.AADHAAR);
        return indices.nullifierIndex;
    }

    function testKycNullifierIndex() external pure returns (uint256) {
        CircuitConstantsV2.DiscloseIndices memory indices = CircuitConstantsV2.getDiscloseIndices(AttestationId.KYC);
        return indices.nullifierIndex;
    }

    // Test SelfUtils.stringToBigInt
    function testStringToBigInt(string memory str) external pure returns (uint256) {
        return SelfUtils.stringToBigInt(str);
    }
}
