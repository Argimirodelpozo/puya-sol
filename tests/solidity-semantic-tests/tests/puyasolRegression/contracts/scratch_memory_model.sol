// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// Memory reference identity probes: every case is wrong under a copy model
/// and right under solc's pointer model.
contract ScratchMemoryModel {
    struct P { uint256 x; uint64 y; bool b; }
    struct Holder { uint256[] items; uint256 n; }

    uint256[] stored;

    // b = a; b[0] = 7; b = c; -> a[0] keeps 7, c[0] untouched, c[1] written through b
    function aliasThenRebind() external pure returns (uint256, uint256, uint256) {
        uint256[] memory a = new uint256[](3);
        uint256[] memory c = new uint256[](3);
        c[0] = 42;
        uint256[] memory b = a;
        b[0] = 7;
        b = c;
        b[1] = 9;
        return (a[0], c[0], c[1]);
    }

    function write(uint256[] memory p, uint256 v) internal pure { p[0] = v; }

    function writeThenReadOther(uint256[] memory p, uint256[] memory q) internal pure returns (uint256) {
        p[0] = 11;
        return q[0];
    }

    // f(a, a): a write through the first parameter is observed through the second
    function repeatedArgument() external pure returns (uint256) {
        uint256[] memory a = new uint256[](2);
        return writeThenReadOther(a, a);
    }

    function mutateThroughCall() external pure returns (uint256) {
        uint256[] memory a = new uint256[](2);
        write(a, 5);
        return a[0];
    }

    function bump(P memory p) internal pure {
        p.x += 1;
        p.y += 2;
        p.b = !p.b;
    }

    function structAlias() external pure returns (uint256, uint64, bool) {
        P memory p = P(1, 2, false);
        P memory q = p;
        bump(q);
        return (p.x, p.y, p.b);
    }

    function pick(uint256[] memory a, uint256[] memory b, bool first) internal pure returns (uint256[] memory) {
        return first ? a : b;
    }

    // a returned reference keeps its identity
    function returnedReference() external pure returns (uint256, uint256) {
        uint256[] memory a = new uint256[](1);
        uint256[] memory b = new uint256[](1);
        uint256[] memory r = pick(a, b, false);
        r[0] = 3;
        return (a[0], b[0]);
    }

    // storage <-> memory conversions are copies
    function storageCopies() external returns (uint256, uint256) {
        stored.push(1);
        stored.push(2);
        uint256[] memory m = stored;
        m[0] = 99;
        uint256 s0 = stored[0];
        stored = m;
        m[1] = 77;
        return (s0, stored[1]);
    }

    // a struct's array member is a shared child
    function nestedShared() external pure returns (uint256, uint256) {
        Holder memory h = Holder(new uint256[](2), 0);
        uint256[] memory child = h.items;
        child[1] = 5;
        h.n = child.length;
        return (h.items[1], h.n);
    }

    // ABI boundary: the caller's array is a copy, the return is a copy
    function echo(uint256[] memory a) external pure returns (uint256[] memory) {
        a[0] += 1;
        return a;
    }

    function fill(uint256 n) external pure returns (uint256 sum) {
        uint256[] memory a = new uint256[](n);
        for (uint256 i = 0; i < a.length; i++) a[i] = i * 2;
        for (uint256 i = 0; i < a.length; i++) sum += a[i];
    }

    // fixed array of narrow ints: `g = f` aliases
    function fixedNarrow() external pure returns (uint8, uint8) {
        uint8[4] memory f;
        f[3] = 200;
        uint8[4] memory g = f;
        g[3] += 1;
        return (f[3], g[3]);
    }

    // reference-typed member slot: assignment stores a pointer
    function nestedAssign() external pure returns (uint256, uint256, uint256) {
        Holder memory h;
        h.items = new uint256[](3);
        h.items[2] = 8;
        uint256[] memory other = new uint256[](1);
        other[0] = 4;
        h.items = other;
        other[0] = 6;
        return (h.items.length, h.items[0], h.n);
    }

    struct Named { string name; uint256 v; }

    function stringMember() external pure returns (string memory, uint256) {
        Named memory n = Named("x", 1);
        n.name = "hello";
        n.v = bytes(n.name).length;
        return (n.name, n.v);
    }

    // uniqueness analysis: `a` stays a value (read-only callee), `b`/`c` share
    function sumReadOnly(uint256[] memory x) internal pure returns (uint256 t) {
        for (uint256 i = 0; i < x.length; i++) t += x[i];
    }

    function uniqueBesideShared() external pure returns (uint256, uint256) {
        uint256[] memory a = new uint256[](2);
        a[0] = 1;
        uint256 s = sumReadOnly(a);
        uint256[] memory b = new uint256[](1);
        uint256[] memory c = b;
        c[0] = 7;
        return (s + a[0], b[0]);
    }

    // a callee returning a fresh object hands over ownership
    function fresh(uint256 n) internal pure returns (uint256[] memory r) {
        r = new uint256[](n);
        r[n - 1] = n;
    }

    function freshBound() external pure returns (uint256) {
        uint256[] memory f = fresh(3);
        f[0] = 5;
        return f[0] + f[2];
    }

    // a callee returning its parameter keeps the caller's identity
    function passThrough(uint256[] memory x) internal pure returns (uint256[] memory) {
        return x;
    }

    function aliasViaReturn() external pure returns (uint256) {
        uint256[] memory a = new uint256[](1);
        uint256[] memory b = passThrough(a);
        b[0] = 9;
        return a[0];
    }

    // named memory return: allocated at entry, mutated in place, returned by pointer
    function makeSeq(uint256 n) internal pure returns (uint256[] memory r) {
        r = new uint256[](n);
        for (uint256 i = 0; i < n; i++) r[i] = i + 1;
    }

    function namedReturn() external pure returns (uint256, uint256) {
        uint256[] memory s = makeSeq(3);
        s[0] = 10;
        return (s[0], s[2]);
    }
}
