// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.2) (utils/Base64.sol)
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/utils/Base64.sol
// Modification: Assembly replaced with pure Solidity implementation (identical output)

pragma solidity ^0.8.20;

/**
 * @dev Provides a set of functions to operate with Base64 strings.
 */
library Base64 {
    bytes private constant TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    /**
     * @dev Converts a `bytes` to its Bytes64 `string` representation.
     */
    function encode(bytes memory data) internal pure returns (string memory) {
        if (data.length == 0) return "";

        // Calculate output length: ceil(dataLen/3) * 4
        uint256 dataLen = data.length;
        uint256 encodedLen = 4 * ((dataLen + 2) / 3);
        bytes memory result = new bytes(encodedLen);

        uint256 i = 0;
        uint256 j = 0;
        while (i < dataLen) {
            uint256 a = uint256(uint8(data[i]));
            uint256 b = i + 1 < dataLen ? uint256(uint8(data[i + 1])) : 0;
            uint256 c = i + 2 < dataLen ? uint256(uint8(data[i + 2])) : 0;

            uint256 triple = (a << 16) | (b << 8) | c;

            result[j]     = TABLE[(triple >> 18) & 0x3F];
            result[j + 1] = TABLE[(triple >> 12) & 0x3F];
            result[j + 2] = i + 1 < dataLen ? TABLE[(triple >> 6) & 0x3F] : bytes1("=");
            result[j + 3] = i + 2 < dataLen ? TABLE[triple & 0x3F] : bytes1("=");

            i += 3;
            j += 4;
        }

        return string(result);
    }
}

// Test contract
contract Base64Test {
    function encode(bytes memory data) external pure returns (string memory) {
        return Base64.encode(data);
    }
}
