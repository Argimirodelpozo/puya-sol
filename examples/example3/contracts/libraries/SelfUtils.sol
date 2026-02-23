// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;
import {SelfStructs} from "./SelfStructs.sol";

library SelfUtils {
    struct UnformattedVerificationConfigV2 {
        uint256 olderThan;
        string[] forbiddenCountries;
        bool ofacEnabled;
    }

    function packForbiddenCountriesList(
        string[] memory forbiddenCountries
    ) internal pure returns (uint256[4] memory output) {
        uint256 MAX_BYTES_IN_FIELD = 31;
        uint256 REQUIRED_CHUNKS = 4;

        bytes memory packedBytes;

        for (uint256 i = 0; i < forbiddenCountries.length; i++) {
            bytes memory countryBytes = bytes(forbiddenCountries[i]);
            require(countryBytes.length == 3, "Invalid country code: must be exactly 3 characters long");
            packedBytes = abi.encodePacked(packedBytes, countryBytes);
        }

        uint256 maxBytes = packedBytes.length;
        uint256 packSize = MAX_BYTES_IN_FIELD;
        uint256 numChunks = (maxBytes + packSize - 1) / packSize;

        for (uint256 i = 0; i < numChunks && i < REQUIRED_CHUNKS; i++) {
            uint256 sum = 0;

            for (uint256 j = 0; j < packSize; j++) {
                uint256 idx = packSize * i + j;
                if (idx < maxBytes) {
                    uint256 value = uint256(uint8(packedBytes[idx]));
                    uint256 shift = 8 * j;
                    sum += value << shift;
                }
            }

            output[i] = sum;
        }

        return output;
    }

    function formatVerificationConfigV2(
        UnformattedVerificationConfigV2 memory unformattedVerificationConfigV2
    ) internal pure returns (SelfStructs.VerificationConfigV2 memory verificationConfigV2) {
        bool[3] memory ofacArray;
        ofacArray[0] = unformattedVerificationConfigV2.ofacEnabled;
        ofacArray[1] = unformattedVerificationConfigV2.ofacEnabled;
        ofacArray[2] = unformattedVerificationConfigV2.ofacEnabled;

        verificationConfigV2 = SelfStructs.VerificationConfigV2({
            olderThanEnabled: unformattedVerificationConfigV2.olderThan > 0,
            olderThan: unformattedVerificationConfigV2.olderThan,
            forbiddenCountriesEnabled: unformattedVerificationConfigV2.forbiddenCountries.length > 0,
            forbiddenCountriesListPacked: packForbiddenCountriesList(
                unformattedVerificationConfigV2.forbiddenCountries
            ),
            ofacEnabled: ofacArray
        });
    }

    function stringToBigInt(string memory str) internal pure returns (uint256) {
        bytes memory strBytes = bytes(str);
        require(strBytes.length <= 31, "String too long for BigInt conversion");

        uint256 result = 0;
        for (uint256 i = 0; i < strBytes.length; i++) {
            require(uint8(strBytes[i]) <= 127, "Non-ASCII character detected");
            result = (result << 8) | uint256(uint8(strBytes[i]));
        }
        return result;
    }

    function addressToHexString(address addr) internal pure returns (string memory) {
        bytes32 value = bytes32(uint256(uint160(addr)));
        bytes memory alphabet = "0123456789abcdef";
        bytes memory str = new bytes(42);

        str[0] = "0";
        str[1] = "x";
        for (uint256 i = 0; i < 20; i++) {
            str[2 + i * 2] = alphabet[uint8(value[i + 12] >> 4)];
            str[3 + i * 2] = alphabet[uint8(value[i + 12] & 0x0f)];
        }

        return string(str);
    }
}
