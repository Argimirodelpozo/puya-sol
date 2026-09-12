// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract ExternalResultFacts {
    uint64 private count;
    function finish(bool ok, bytes memory data) internal pure returns (bytes memory, uint256 n) {
        require(ok);
        assembly { n := returndatasize() }
        return (data, n);
    }
    function identity(bytes memory data) external view returns (bytes memory, uint256) {
        (bool ok, bytes memory result) = address(4).staticcall(data);
        return finish(ok, result);
    }
    function sha(bytes memory data) external view returns (bytes memory, uint256) {
        (bool ok, bytes memory result) = address(2).staticcall(data);
        return finish(ok, result);
    }
    function recovery(bytes memory data) external view returns (bytes memory, uint256) {
        (bool ok, bytes memory result) = address(1).staticcall(data);
        return finish(ok, result);
    }
    function add(bytes memory data) external view returns (bytes memory, uint256) {
        (bool ok, bytes memory result) = address(6).staticcall(data);
        return finish(ok, result);
    }
    function mul(bytes memory data) external view returns (bytes memory, uint256) {
        (bool ok, bytes memory result) = address(7).staticcall(data);
        return finish(ok, result);
    }
    function pair(bytes memory data) external view returns (bytes memory, uint256) {
        (bool ok, bytes memory result) = address(8).staticcall(data);
        return finish(ok, result);
    }
    function modexp(bytes memory data) external view returns (bytes memory, uint256) {
        (bool ok, bytes memory result) = address(5).staticcall(data);
        return finish(ok, result);
    }
    function nothing() external { ++count; }
    function signedValue() external returns (int8) { ++count; return -7; }
    function mixed() external returns (int16, uint16, bytes memory) {
        ++count; return (-9, 1234, hex"123456");
    }
    function transition() external returns (uint256 emptySize, uint256 signedSize, int8 value, uint64 calls) {
        count = 0;
        assembly { pop(staticcall(gas(), 4, 128, 32, 160, 32)) }
        this.nothing();
        assembly { emptySize := returndatasize() }
        value = this.signedValue();
        assembly { signedSize := returndatasize() }
        calls = count;
    }
    function lowSelf() external returns (bytes memory, uint256, uint64) {
        count = 0;
        (bool ok, bytes memory result) = address(this).call(abi.encodeCall(this.mixed, ()));
        require(ok);
        uint256 n;
        assembly { n := returndatasize() }
        return (result, n, count);
    }
    function pointerSelf(bool first) external returns (uint256 n, int8 value, uint64 calls) {
        count = 0;
        function() external returns (int8) pointer = first ? this.signedValue : this.signedOther;
        value = pointer();
        assembly { n := returndatasize() }
        calls = count;
    }
    function signedOther() external returns (int8) { ++count; return -8; }
}
