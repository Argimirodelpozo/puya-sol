pragma solidity ^0.8.20;

struct MemoryItem { uint256 x; }

library MemoryIdentityLibrary {
    function identity(MemoryItem memory p) internal pure returns (MemoryItem memory) { return p; }
    function publicIdentity(MemoryItem memory p) public pure returns (MemoryItem memory) { p.x = 7; return p; }
    function publicPair(MemoryItem memory p) public pure returns (MemoryItem memory, MemoryItem memory) {
        return (p, p);
    }
}

abstract contract MemoryIdentityBase {
    function choose(MemoryItem memory a, MemoryItem memory b) internal pure virtual returns (MemoryItem memory) {
        return a;
    }
}

contract ReturnedMemoryIdentity is MemoryIdentityBase {
    using MemoryIdentityLibrary for MemoryItem;
    uint256 private calls;
    constructor() {}

    function identity(MemoryItem memory p) internal pure returns (MemoryItem memory) { return p; }
    function directWrite(MemoryItem memory p) internal pure { MemoryItem memory q = p; q.x = 9; }
    function returnedWrite(MemoryItem memory p) internal pure { MemoryItem memory q = identity(p); q.x = 9; }
    function choose(MemoryItem memory a, MemoryItem memory b) internal pure override returns (MemoryItem memory) {
        return b;
    }
    function named(MemoryItem memory p) internal pure returns (MemoryItem memory q) { q = p; }
    function pair(MemoryItem memory p) internal pure returns (MemoryItem memory, MemoryItem memory) {
        return (p, p);
    }
    function forward(MemoryItem memory p) internal pure returns (MemoryItem memory, MemoryItem memory) {
        return pair(p);
    }
    function counted(MemoryItem memory p) internal returns (MemoryItem memory) { ++calls; return p; }
    function direct() external pure returns (uint256) {
        MemoryItem memory p = MemoryItem(5); directWrite(p); return p.x;
    }
    function throughReturn() external pure returns (uint256) {
        MemoryItem memory p = MemoryItem(5); returnedWrite(p); return p.x;
    }
    function retainedAlias() external pure returns (uint256, uint256) {
        MemoryItem memory p = MemoryItem(5); MemoryItem memory q = identity(p);
        p = MemoryItem(7); q.x = 9; return (p.x, q.x);
    }
    function localReturnedAlias() external pure returns (uint256, uint256) {
        MemoryItem memory p = MemoryItem(5); MemoryItem memory q = identity(p);
        q.x = 9; return (p.x, q.x);
    }
    function duplicateReturn() external pure returns (uint256, uint256, uint256) {
        MemoryItem memory p = MemoryItem(5);
        (MemoryItem memory a, MemoryItem memory b) = forward(p);
        a.x = 9; return (p.x, a.x, b.x);
    }
    function conditionalTuple(bool flag) external pure returns (uint256, uint256, uint256) {
        MemoryItem memory p = MemoryItem(5); MemoryItem memory q = MemoryItem(7);
        (MemoryItem memory a, MemoryItem memory b) = flag ? pair(p) : pair(q);
        a.x = 9; return (p.x, q.x, b.x);
    }
    function conditionalFreshTuple(bool flag) external pure returns (uint256, uint256) {
        MemoryItem memory p = MemoryItem(5);
        (MemoryItem memory a, MemoryItem memory b) = flag ? pair(p) : (MemoryItem(7), MemoryItem(8));
        a.x = 9; return (p.x, b.x);
    }
    function empty() internal pure returns (MemoryItem memory) {}
    function emptyPair() internal pure returns (MemoryItem memory, MemoryItem memory) {}
    function defaults() external pure returns (uint256, uint256, uint256, uint256) {
        MemoryItem memory p = empty(); MemoryItem memory q = empty();
        (MemoryItem memory a, MemoryItem memory b) = emptyPair();
        p.x = 9; a.x = 7; return (p.x, q.x, a.x, b.x);
    }
    function conditional(bool flag) external pure returns (uint256, uint256) {
        MemoryItem memory p = MemoryItem(5);
        MemoryItem memory q = flag ? named(p) : MemoryItem(7);
        q.x = 9; return (p.x, q.x);
    }
    function dispatch(bool flag) external pure returns (uint256, uint256) {
        MemoryItem memory a = MemoryItem(5); MemoryItem memory b = MemoryItem(7);
        function(MemoryItem memory, MemoryItem memory) internal pure returns (MemoryItem memory) fn = choose;
        if (flag) fn = first;
        MemoryItem memory q = fn(a, b); q.x = 9; return (a.x, b.x);
    }
    function first(MemoryItem memory a, MemoryItem memory) internal pure returns (MemoryItem memory) { return a; }
    function libraryAlias() external pure returns (uint256) {
        MemoryItem memory p = MemoryItem(5); MemoryItem memory q = p.identity(); q.x = 9; return p.x;
    }
    function libraryCopy() external pure returns (uint256, uint256, uint256) {
        MemoryItem memory p = MemoryItem(5); MemoryItem memory q = p.publicIdentity();
        uint256 returned = q.x; q.x = 9; return (p.x, returned, q.x);
    }
    function libraryPairCopy() external pure returns (uint256, uint256, uint256) {
        MemoryItem memory p = MemoryItem(5);
        (MemoryItem memory a, MemoryItem memory b) = p.publicPair();
        a.x = 9; return (p.x, a.x, b.x);
    }
    function publicIdentity(MemoryItem memory p) public pure returns (MemoryItem memory) { return p; }
    function publicAlias() external pure returns (uint256) {
        MemoryItem memory p = MemoryItem(5); MemoryItem memory q = publicIdentity(p); q.x = 9; return p.x;
    }
    function publicPointer() external view returns (uint256, uint256, uint256) {
        MemoryItem memory p = MemoryItem(5);
        function(MemoryItem memory) internal pure returns (MemoryItem memory) internalFn = publicIdentity;
        function(MemoryItem memory) external pure returns (MemoryItem memory) externalFn = this.publicIdentity;
        MemoryItem memory a = internalFn(p); MemoryItem memory b = externalFn(p);
        a.x = 9; return (p.x, a.x, b.x);
    }
    function directDestination() external returns (uint256, uint256) {
        calls = 0; MemoryItem memory p = MemoryItem(5); counted(p).x = 9; return (p.x, calls);
    }
    function argumentOnce() external returns (uint256, uint256) {
        calls = 0; MemoryItem memory p = MemoryItem(5); returnedWrite(counted(p)); return (p.x, calls);
    }
    modifier afterBody(MemoryItem memory p) { _; p.x += 1; }
    function modified(MemoryItem memory p) internal pure afterBody(p) returns (MemoryItem memory r) { r = p; }
    function modifierAlias() external pure returns (uint256, uint256) {
        MemoryItem memory p = MemoryItem(5); MemoryItem memory q = modified(p); q.x += 3; return (p.x, q.x);
    }
}

contract ReturnedMemoryArrays {
    struct Holder { uint256[] values; }
    function identity(uint256[] memory p) internal pure returns (uint256[] memory) { return p; }
    function bytesIdentity(bytes memory p) internal pure returns (bytes memory) { return p; }
    function member(Holder memory p) internal pure returns (uint256[] memory) { return p.values; }
    function fresh() internal pure returns (uint256[] memory p) { p = new uint256[](2); p[0] = 7; }
    function arrays() external pure returns (uint256, uint256, uint256) {
        uint256[] memory p = new uint256[](2); uint256[] memory q = identity(p);
        q[0] = 9; uint256[] memory r = fresh(); return (p[0], q[0], r[0]);
    }
    function nested() external pure returns (uint256) {
        Holder memory p = Holder(new uint256[](2));
        uint256[] memory q = member(p); q[1] = 9; return p.values[1];
    }
    function bytesAlias() external pure returns (bytes memory) {
        bytes memory p = hex"010203"; bytes memory q = bytesIdentity(p); q[1] = 0xff; return p;
    }
}
