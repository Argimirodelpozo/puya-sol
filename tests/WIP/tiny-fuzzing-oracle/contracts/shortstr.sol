// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// OZ ShortStrings.toString: mstore(str, len) is a LENGTH-WORD write (resize).
contract ShortStr {
    function toStr(bytes32 sstr, uint256 len) external pure returns (string memory) {
        require(len <= 32);
        string memory str = new string(32);
        assembly {
            mstore(str, len)
            mstore(add(str, 0x20), sstr)
        }
        return str;
    }
    // length write AFTER the data write (reverse order)
    function toStrRev(bytes32 sstr, uint256 len) external pure returns (string memory) {
        require(len <= 32);
        string memory str = new string(32);
        assembly {
            mstore(add(str, 0x20), sstr)
            mstore(str, len)
        }
        return str;
    }
    // GROW past the allocation
    function grow(uint256 len) external pure returns (uint256) {
        require(len <= 64);
        bytes memory b = new bytes(8);
        assembly { mstore(b, len) }
        return b.length;
    }
    function shrinkContent(bytes32 w, uint256 len) external pure returns (bytes memory) {
        require(len <= 32);
        bytes memory b = new bytes(32);
        assembly {
            mstore(add(b, 0x20), w)
            mstore(b, len)
        }
        return b;
    }
}
