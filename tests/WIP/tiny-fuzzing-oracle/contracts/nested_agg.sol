// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Differential battery: NESTED aggregate read/write round-trips. Each function builds a
// nested structure and returns an observable encoding several field reads, so a layout /
// indexing / deep-copy bug shows up as a wrong value. Self-contained (no persistence).
contract NestedAgg {
    struct Inner { uint256 a; uint256 b; }
    struct Outer { uint256 id; Inner inner; uint256[] list; }
    Outer o;
    uint256[][] grid;
    Inner[] items;
    mapping(uint256 => Inner) mi;

    struct WithMap { uint256 id; mapping(uint256 => uint256) bal; }
    mapping(uint256 => WithMap) wm;

    // struct nested in a struct (storage). correct=33.
    function structInStruct() external returns (uint256) {
        o.id = 1;
        o.inner.a = 33;
        return o.inner.a;
    }

    // dynamic array field inside a struct: push + length + index. correct=222.
    function arrayInStruct() external returns (uint256) {
        delete o.list;
        o.list.push(11);
        o.list.push(22);
        return o.list[1] * 10 + o.list.length;
    }

    // array of structs: mutate one element's field, read a neighbor's. correct=101.
    function structInArray() external returns (uint256) {
        delete items;
        items.push(Inner(1, 2));
        items.push(Inner(3, 4));
        items[1].a = 99;
        return items[1].a + items[0].b;
    }

    // array of arrays (uint[][]): nested push + index + length. correct=82.
    function arrayOfArrays() external returns (uint256) {
        delete grid;
        grid.push();
        grid[0].push(7);
        grid[0].push(8);
        return grid[0][1] * 10 + grid[0].length;
    }

    // mapping of structs. correct=121.
    function mappingOfStruct() external returns (uint256) {
        mi[3].a = 55;
        mi[3].b = 66;
        return mi[3].a + mi[3].b;
    }

    // whole struct read out of a mapping into memory (deep copy). correct=1234.
    function structFromMappingToMemory() external returns (uint256) {
        mi[7] = Inner(12, 34);
        Inner memory cp = mi[7];
        cp.a = 0;                 // mutate the copy (must not affect storage)
        return mi[7].a * 100 + mi[7].b;
    }

    // struct containing a nested mapping inside a mapping (the V4/AAVE pattern). correct=1209.
    function structWithMapping() external returns (uint256) {
        wm[1].id = 9;
        wm[1].bal[100] = 500;
        wm[1].bal[200] = 700;
        return wm[1].bal[100] + wm[1].bal[200] + wm[1].id;
    }

    // fixed-size nested array (uint[2][3] static). correct=4321.
    function fixedNested() external pure returns (uint256) {
        uint256[2][3] memory g;
        g[0][0] = 1; g[0][1] = 2;
        g[2][1] = 4;
        return g[2][1] * 1000 + g[0][1] * 100 + g[0][0] * 10 + 1;
    }
}
