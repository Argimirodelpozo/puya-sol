// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract CallConversions {
    enum Choice { A, B }

    function fixedValues(bytes calldata input, uint128 n, int168 s)
        external pure returns (bytes4, bytes4, bytes16, bytes21, bytes20)
    {
        bytes20 who = hex"00112233445566778899aabbccddeeff00112233";
        return (bytes4("abc"), bytes4(input), bytes16(n), bytes21(uint168(s)), bytes20(address(who)));
    }

    function views(bytes memory input) external pure returns (bytes memory) {
        return bytes(string(input));
    }

    function ordinal(uint256 n) external pure returns (uint8) { return uint8(Choice(n)); }
}
