// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

abstract contract CalldataReturnBase {
    function identity(bytes calldata x) internal pure virtual returns (bytes calldata) { return x; }
}

contract CalldataReturns is CalldataReturnBase {
    struct Record { uint256 value; }
    modifier around() { _; }
    function identity(bytes calldata x) internal pure override returns (bytes calldata) { return x; }
    function tail(bytes calldata x) internal pure returns (bytes calldata) { return x[1:]; }
    function named(bytes calldata x) internal pure around returns (bytes calldata y) { y = x; }
    function pair(bytes calldata x, bytes calldata y) internal pure returns (bytes calldata, bytes calldata) {
        return (identity(x), identity(y));
    }
    function choose(bytes calldata x, bytes calldata y, bool flag) internal pure returns (bytes calldata) {
        return flag ? identity(x) : identity(y);
    }
    function fake(bytes calldata, uint256 off, uint256 len) internal pure returns (bytes calldata y) {
        assembly { y.offset := off y.length := len }
    }
    function stringIdentity(string calldata x) internal pure returns (string calldata) { return x; }
    function stringView(bytes calldata x) internal pure returns (string calldata) { return string(x); }
    function arrayIdentity(uint256[] calldata x) internal pure returns (uint256[] calldata) { return x; }
    function fixedIdentity(uint256[2] calldata x) internal pure returns (uint256[2] calldata) { return x; }
    function recordIdentity(Record calldata x) internal pure returns (Record calldata) { return x; }
    function touch(bytes calldata x, uint256[] memory counter) internal pure returns (bytes calldata) {
        counter[0] += 1;
        return x;
    }
    function length(bytes calldata x) external pure returns (uint256 n) {
        bytes calldata y = ((identity(x)));
        assembly { n := y.length }
    }
    function copy(bytes calldata x) external pure returns (bytes memory) { return identity(x); }
    function publicReference(bytes calldata x) public pure returns (bytes calldata) { return identity(x); }
    function namedLength(bytes calldata x) external pure returns (uint256 n) {
        bytes calldata y = named(x);
        assembly { n := y.length }
    }
    function sliced(bytes calldata x) external pure returns (uint256 off, uint256 len, bytes1 first) {
        bytes calldata y = tail(x);
        assembly { off := sub(y.offset, x.offset) len := y.length }
        first = y[0];
    }
    function indirect(bytes calldata x, bool flag) external pure returns (uint256 off, uint256 len) {
        function(bytes calldata) internal pure returns (bytes calldata) f = flag ? identity : tail;
        bytes calldata y = f(x);
        assembly { off := sub(y.offset, x.offset) len := y.length }
    }
    function rebound(bytes calldata x, bytes calldata y, bool flag) external pure returns (uint256 n) {
        bytes calldata z = identity(x);
        z = choose(z, y, flag);
        assembly { n := z.length }
    }
    function tupleReturn(bytes calldata x, bytes calldata y, bool flag) external pure returns (uint256 a, uint256 b) {
        (bytes calldata p, bytes calldata q) = flag ? pair(x, y) : pair(y, x);
        assembly { a := p.length b := q.length }
    }
    function tupleCopy(bytes calldata x, bytes calldata y) external pure returns (bytes memory a, bytes memory b) {
        (a, b) = pair(x, y);
    }
    function forged(bytes calldata x, uint256 off, uint256 len) external pure returns (uint256 a, uint256 b) {
        bytes calldata y = fake(x, off, len);
        assembly { a := y.offset b := y.length }
    }
    function forgedRead(bytes calldata x, uint256 off, uint256 len) external pure returns (bytes1) {
        return fake(x, off, len)[0];
    }
    function discardForged(bytes calldata x) external pure returns (uint256) {
        fake(x, type(uint256).max, type(uint256).max);
        return 7;
    }
    function discardBounds(bytes calldata x) external pure returns (uint256) {
        tail(x);
        return 7;
    }
    function stringLength(string calldata x) external pure returns (uint256 n) {
        string calldata y = stringIdentity(x);
        assembly { n := y.length }
    }
    function viewLength(bytes calldata x) external pure returns (uint256 n, uint256 off) {
        bytes calldata y = bytes(stringView(x));
        assembly { n := y.length off := sub(y.offset, x.offset) }
    }
    function arrayLength(uint256[] calldata x) external pure returns (uint256 n, uint256 first) {
        uint256[] calldata y = arrayIdentity(x);
        assembly { n := y.length }
        first = y[0];
    }
    function fixedArray(uint256[2] calldata x) external pure returns (uint256, uint256) {
        uint256[2] calldata y = fixedIdentity(x);
        return (y[0], y[1]);
    }
    function records(Record[] calldata x) external pure returns (uint256) {
        Record calldata y = recordIdentity(x[0]);
        return y.value;
    }
    function memoryWriteBack(bytes calldata x) external pure returns (uint256, uint256) {
        uint256[] memory counter = new uint256[](1);
        bytes calldata y = touch(x, counter);
        return (counter[0], y.length);
    }
}
