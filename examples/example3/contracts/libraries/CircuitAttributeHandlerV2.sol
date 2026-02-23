// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

import {Formatter} from "./Formatter.sol";
import {AttestationId} from "../constants/AttestationId.sol";
import {SelfStructs} from "./SelfStructs.sol";

/**
 * @title UnifiedAttributeHandler Library
 * @notice Provides functions for extracting and formatting attributes from both passport and ID card byte arrays.
 * @dev Utilizes the Formatter library for converting and formatting specific fields.
 */
library CircuitAttributeHandlerV2 {
    /**
     * @dev Reverts when the provided character codes array does not contain enough data to extract an attribute.
     */
    error InsufficientCharcodeLen();

    /**
     * @notice Structure containing field positions for a specific attestation type.
     */
    struct FieldPositions {
        uint256 issuingStateStart;
        uint256 issuingStateEnd;
        uint256 nameStart;
        uint256 nameEnd;
        uint256 documentNumberStart;
        uint256 documentNumberEnd;
        uint256 nationalityStart;
        uint256 nationalityEnd;
        uint256 dateOfBirthStart;
        uint256 dateOfBirthEnd;
        uint256 genderStart;
        uint256 genderEnd;
        uint256 expiryDateStart;
        uint256 expiryDateEnd;
        uint256 olderThanStart;
        uint256 olderThanEnd;
        uint256 ofacStart;
        uint256 ofacEnd;
    }

    /**
     * @notice Returns the field positions for a given attestation type.
     * @param attestationId The attestation identifier.
     * @return positions The FieldPositions struct containing all relevant positions (inclusive).
     */
    function getFieldPositions(bytes32 attestationId) internal pure returns (FieldPositions memory positions) {
        if (attestationId == AttestationId.E_PASSPORT) {
            return
                FieldPositions({
                    issuingStateStart: 2,
                    issuingStateEnd: 4,
                    nameStart: 5,
                    nameEnd: 43,
                    documentNumberStart: 44,
                    documentNumberEnd: 52,
                    nationalityStart: 54,
                    nationalityEnd: 56,
                    dateOfBirthStart: 57,
                    dateOfBirthEnd: 62,
                    genderStart: 64,
                    genderEnd: 64,
                    expiryDateStart: 65,
                    expiryDateEnd: 70,
                    olderThanStart: 88,
                    olderThanEnd: 89,
                    ofacStart: 90,
                    ofacEnd: 92
                });
        } else if (attestationId == AttestationId.EU_ID_CARD) {
            return
                FieldPositions({
                    issuingStateStart: 2,
                    issuingStateEnd: 4,
                    nameStart: 60,
                    nameEnd: 89,
                    documentNumberStart: 5,
                    documentNumberEnd: 13,
                    nationalityStart: 45,
                    nationalityEnd: 47,
                    dateOfBirthStart: 30,
                    dateOfBirthEnd: 35,
                    genderStart: 37,
                    genderEnd: 37,
                    expiryDateStart: 38,
                    expiryDateEnd: 43,
                    olderThanStart: 90,
                    olderThanEnd: 91,
                    ofacStart: 92,
                    ofacEnd: 93
                });
        } else if (attestationId == AttestationId.AADHAAR) {
            return
                FieldPositions({
                    issuingStateStart: 81,
                    issuingStateEnd: 111,
                    nameStart: 9,
                    nameEnd: 70,
                    documentNumberStart: 71,
                    documentNumberEnd: 74,
                    nationalityStart: 999,
                    nationalityEnd: 999,
                    dateOfBirthStart: 1,
                    dateOfBirthEnd: 8,
                    genderStart: 0,
                    genderEnd: 0,
                    expiryDateStart: 999,
                    expiryDateEnd: 999,
                    olderThanStart: 118,
                    olderThanEnd: 118,
                    ofacStart: 116,
                    ofacEnd: 117
                });
        } else if (attestationId == AttestationId.KYC) {
            return
                FieldPositions({
                    issuingStateStart: 999,
                    issuingStateEnd: 999,
                    nameStart: 78,
                    nameEnd: 141,
                    documentNumberStart: 30,
                    documentNumberEnd: 61,
                    nationalityStart: 0,
                    nationalityEnd: 2,
                    dateOfBirthStart: 142,
                    dateOfBirthEnd: 149,
                    genderStart: 194,
                    genderEnd: 194,
                    expiryDateStart: 70,
                    expiryDateEnd: 77,
                    olderThanStart: 297,
                    olderThanEnd: 297,
                    ofacStart: 295,
                    ofacEnd: 296
                });
        } else {
            revert("Invalid attestation ID");
        }
    }

    function getIssuingState(bytes32 attestationId, bytes memory charcodes) internal pure returns (string memory) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        return extractStringAttribute(charcodes, positions.issuingStateStart, positions.issuingStateEnd);
    }

    function getName(bytes32 attestationId, bytes memory charcodes) internal pure returns (string[] memory) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        if (attestationId == AttestationId.AADHAAR) {
            string memory fullName = extractStringAttribute(charcodes, positions.nameStart, positions.nameEnd);
            string[] memory nameParts = new string[](2);
            nameParts[0] = fullName;
            return nameParts;
        }
        return Formatter.formatName(extractStringAttribute(charcodes, positions.nameStart, positions.nameEnd));
    }

    function getDocumentNumber(bytes32 attestationId, bytes memory charcodes) internal pure returns (string memory) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        return extractStringAttribute(charcodes, positions.documentNumberStart, positions.documentNumberEnd);
    }

    function getNationality(bytes32 attestationId, bytes memory charcodes) internal pure returns (string memory) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        return extractStringAttribute(charcodes, positions.nationalityStart, positions.nationalityEnd);
    }

    function getDateOfBirth(bytes32 attestationId, bytes memory charcodes) internal pure returns (string memory) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        return
            Formatter.formatDate(
                extractStringAttribute(charcodes, positions.dateOfBirthStart, positions.dateOfBirthEnd)
            );
    }

    function getDateOfBirthFullYear(
        bytes32 attestationId,
        bytes memory charcodes
    ) internal pure returns (string memory) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        return
            Formatter.formatDateFullYear(
                extractStringAttribute(charcodes, positions.dateOfBirthStart, positions.dateOfBirthEnd)
            );
    }

    function getGender(bytes32 attestationId, bytes memory charcodes) internal pure returns (string memory) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        return extractStringAttribute(charcodes, positions.genderStart, positions.genderEnd);
    }

    function getExpiryDate(bytes32 attestationId, bytes memory charcodes) internal pure returns (string memory) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        return
            Formatter.formatDate(extractStringAttribute(charcodes, positions.expiryDateStart, positions.expiryDateEnd));
    }

    function getExpiryDateFullYear(
        bytes32 attestationId,
        bytes memory charcodes
    ) internal pure returns (string memory) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        return
            Formatter.formatDateFullYear(
                extractStringAttribute(charcodes, positions.expiryDateStart, positions.expiryDateEnd)
            );
    }

    function getOlderThan(bytes32 attestationId, bytes memory charcodes) internal pure returns (uint256) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        if (attestationId == AttestationId.KYC) {
            return
                Formatter.numAsciiToUint(uint8(charcodes[positions.olderThanStart])) * 100 +
                Formatter.numAsciiToUint(uint8(charcodes[positions.olderThanStart + 1])) * 10 +
                Formatter.numAsciiToUint(uint8(charcodes[positions.olderThanStart + 2]));
        }
        return
            Formatter.numAsciiToUint(uint8(charcodes[positions.olderThanStart])) * 10 +
            Formatter.numAsciiToUint(uint8(charcodes[positions.olderThanStart + 1]));
    }

    function getDocumentNoOfac(bytes32 attestationId, bytes memory charcodes) internal pure returns (bool) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        return uint8(charcodes[positions.ofacStart]) == 1;
    }

    function getNameAndDobOfac(bytes32 attestationId, bytes memory charcodes) internal pure returns (bool) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        if (attestationId == AttestationId.E_PASSPORT) {
            return uint8(charcodes[positions.ofacStart + 1]) == 1;
        } else {
            return uint8(charcodes[positions.ofacStart]) == 1;
        }
    }

    function getNameAndYobOfac(bytes32 attestationId, bytes memory charcodes) internal pure returns (bool) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        if (attestationId == AttestationId.E_PASSPORT) {
            return uint8(charcodes[positions.ofacStart + 2]) == 1;
        } else {
            return uint8(charcodes[positions.ofacStart + 1]) == 1;
        }
    }

    function compareOfac(
        bytes32 attestationId,
        bytes memory charcodes,
        bool checkDocumentNo,
        bool checkNameAndDob,
        bool checkNameAndYob
    ) internal pure returns (bool) {
        bool documentNoResult = true;
        bool nameAndDobResult = true;
        bool nameAndYobResult = true;

        if (checkDocumentNo && attestationId == AttestationId.E_PASSPORT) {
            documentNoResult = getDocumentNoOfac(attestationId, charcodes);
        }

        if (checkNameAndDob) {
            nameAndDobResult = getNameAndDobOfac(attestationId, charcodes);
        }

        if (checkNameAndYob) {
            nameAndYobResult = getNameAndYobOfac(attestationId, charcodes);
        }

        return documentNoResult && nameAndDobResult && nameAndYobResult;
    }

    function compareOlderThan(
        bytes32 attestationId,
        bytes memory charcodes,
        uint256 olderThan
    ) internal pure returns (bool) {
        return getOlderThan(attestationId, charcodes) >= olderThan;
    }

    function compareOlderThanNumeric(
        bytes32 attestationId,
        bytes memory charcodes,
        uint256 olderThan
    ) internal pure returns (bool) {
        FieldPositions memory positions = getFieldPositions(attestationId);
        uint256 extractedAge = uint8(charcodes[positions.olderThanStart]);
        return extractedAge >= olderThan;
    }

    function extractStringAttribute(
        bytes memory charcodes,
        uint256 start,
        uint256 end
    ) internal pure returns (string memory) {
        if (charcodes.length <= end) {
            revert InsufficientCharcodeLen();
        }
        bytes memory attributeBytes = new bytes(end - start + 1);
        for (uint256 i = start; i <= end; i++) {
            attributeBytes[i - start] = charcodes[i];
        }
        return string(attributeBytes);
    }

    // ====================================================
    // Legacy Functions (for backward compatibility)
    // ====================================================

    function getPassportNumber(bytes memory charcodes) internal pure returns (string memory) {
        return getDocumentNumber(AttestationId.E_PASSPORT, charcodes);
    }

    function getPassportNoOfac(bytes memory charcodes) internal pure returns (uint256) {
        return getDocumentNoOfac(AttestationId.E_PASSPORT, charcodes) ? 1 : 0;
    }
}
