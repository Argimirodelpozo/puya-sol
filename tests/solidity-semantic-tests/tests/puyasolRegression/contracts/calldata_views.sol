// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

contract CalldataViews {
    modifier around() { _; }
    function data() internal pure returns (bytes calldata) { return msg.data; }
    function named() internal pure around returns (bytes calldata result) { result = msg.data; }
    function identity(bytes calldata x) internal pure returns (bytes calldata) { return x; }
    function pair(bytes calldata x) internal pure returns (bytes calldata, bytes calldata) { return (msg.data, x); }
    function empty() internal pure returns (bytes calldata result) { assembly { result.length := 0 } }
    function emptyModified() internal pure around returns (bytes calldata result) { assembly { result.length := 0 } }
    function message(uint256, bool) external pure returns (bool, uint256) {
        bytes calldata result = data();
        return (keccak256(result) == keccak256(msg.data), result.length);
    }
    function namedMessage(uint256, bool) external pure returns (bool, uint256) {
        bytes calldata result = named();
        return (keccak256(result) == keccak256(msg.data), result.length);
    }
    function pointerMessage(uint256, bool flag) external pure returns (bool, uint256) {
        function() internal pure returns (bytes calldata) f = flag ? data : named;
        bytes calldata result = f();
        return (keccak256(result) == keccak256(msg.data), result.length);
    }
    function mixed(bytes calldata x, bool messageView) external pure returns (bool, uint256) {
        bytes calldata result = messageView ? data() : identity(x);
        bytes memory expected = messageView ? msg.data : x;
        return (keccak256(identity(result)) == keccak256(expected), result.length);
    }
    function tupleViews(bytes calldata x, bool) external pure returns (bool, bool) {
        (bytes calldata a, bytes calldata b) = pair(x);
        return (keccak256(a) == keccak256(msg.data), keccak256(b) == keccak256(x));
    }
    function rebind(uint256, bool) external pure returns (bool, uint256) {
        bytes calldata result = msg.data;
        result = identity(data());
        return (keccak256(result) == keccak256(msg.data), result.length);
    }
    function defaults(uint256, bool) external pure returns (uint256 a, uint256 b, uint256 c, uint256 n) {
        bytes calldata x = empty();
        bytes calldata y = emptyModified();
        bytes calldata z;
        assembly { z.length := 0 a := x.offset b := y.offset c := z.offset n := add(add(x.length, y.length), z.length) }
    }
    function resizeMessage(uint256, bool) external pure returns (bool, uint256) {
        bytes calldata result = msg.data;
        assembly { result.length := 37 }
        return (keccak256(result) == keccak256(msg.data[:37]), result.length);
    }
}
