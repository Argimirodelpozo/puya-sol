// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // b[i++] |= 0x10 : index i++ once (i:0->1), b[0]=0x11. Expect (0x11,0x02,0x04,1).
    function bytesElemCompound() external pure returns (bytes1, bytes1, bytes1, uint256) {
        bytes memory b = new bytes(3);
        b[0] = 0x01; b[1] = 0x02; b[2] = 0x04;
        uint256 i = 0;
        b[i++] |= 0x10;
        return (b[0], b[1], b[2], i);
    }
}
