pragma solidity ^0.8.20;

contract BytesStorageHeaders {
    bytes b;
    function read(uint256 word) external returns (bytes memory) {
        assembly { sstore(b.slot, word) }
        return b;
    }
    function replace(uint256 word) external returns (bytes memory) {
        assembly { sstore(b.slot, word) }
        b = hex"1122";
        return b;
    }
}
