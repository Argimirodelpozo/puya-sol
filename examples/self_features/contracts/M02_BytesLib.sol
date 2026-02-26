// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

library BytesLib {
    /// Convert an ASCII digit byte to its numeric value (0-9).
    /// Reverts if the byte is not an ASCII digit (0x30-0x39).
    function asciiDigitToUint(uint256 asciiCode) internal pure returns (uint256) {
        require(asciiCode >= 48 && asciiCode <= 57, "not an ASCII digit");
        return asciiCode - 48;
    }

    /// Concatenate two byte arrays using abi.encodePacked.
    function concatBytes(bytes memory a, bytes memory b) internal pure returns (bytes memory) {
        return abi.encodePacked(a, b);
    }
}

contract BytesLibTest {
    function testAsciiDigit(uint256 code) external pure returns (uint256) {
        return BytesLib.asciiDigitToUint(code);
    }

    function testConcat(bytes memory a, bytes memory b) external pure returns (bytes memory) {
        return BytesLib.concatBytes(a, b);
    }
}
