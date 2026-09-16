pragma solidity ^0.8.20;

contract EbBytes {
    function resize(bytes memory value) external pure returns (bytes4) { return bytes4(value); }
    function mixed(bytes2 a, bytes4 b) external pure returns (bytes4, bytes4, bytes4, bool, bool) {
        return (a | b, a & b, a ^ b, a == b, a < b);
    }
}
