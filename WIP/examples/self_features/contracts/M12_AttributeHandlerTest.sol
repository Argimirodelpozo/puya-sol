// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

import "constants/AttestationId.sol";
import "libraries/CircuitAttributeHandlerV2.sol";

/**
 * M12: Tests CircuitAttributeHandlerV2 core functions.
 * Exercises: bytes→string, new bytes(N), bytes element write, struct field access,
 * string extraction from byte arrays, multi-library call chains.
 */
contract AttributeHandlerTest {
    // Test extractStringAttribute directly
    function testExtractString(bytes memory charcodes, uint256 start, uint256 end) external pure returns (string memory) {
        return CircuitAttributeHandlerV2.extractStringAttribute(charcodes, start, end);
    }

    // Test getIssuingState with passport charcodes
    function testGetIssuingState(bytes memory charcodes) external pure returns (string memory) {
        return CircuitAttributeHandlerV2.getIssuingState(AttestationId.E_PASSPORT, charcodes);
    }

    // Test getDocumentNoOfac
    function testGetDocumentNoOfac(bytes memory charcodes) external pure returns (bool) {
        return CircuitAttributeHandlerV2.getDocumentNoOfac(AttestationId.E_PASSPORT, charcodes);
    }

    // Test getOlderThan for passport
    function testGetOlderThan(bytes memory charcodes) external pure returns (uint256) {
        return CircuitAttributeHandlerV2.getOlderThan(AttestationId.E_PASSPORT, charcodes);
    }

    // Test compareOlderThan
    function testCompareOlderThan(bytes memory charcodes, uint256 minAge) external pure returns (bool) {
        return CircuitAttributeHandlerV2.compareOlderThan(AttestationId.E_PASSPORT, charcodes, minAge);
    }

    // Test getFieldPositions returns correct values for passport
    function testPassportNameStart() external pure returns (uint256) {
        CircuitAttributeHandlerV2.FieldPositions memory pos = CircuitAttributeHandlerV2.getFieldPositions(AttestationId.E_PASSPORT);
        return pos.nameStart;
    }

    function testPassportNameEnd() external pure returns (uint256) {
        CircuitAttributeHandlerV2.FieldPositions memory pos = CircuitAttributeHandlerV2.getFieldPositions(AttestationId.E_PASSPORT);
        return pos.nameEnd;
    }
}
