// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Differential battery: storage/memory DATA-LOCATION semantics (alias vs copy).
// Each function is self-contained — it sets up, performs the location op, and returns an
// observable that DIFFERS if the alias/copy semantics are wrong — so .call()/simulate on
// both sides (no persistence needed) reveals any divergence. Run via fuzz_evm.py.
contract LocAliasing {
    struct S { uint256 x; uint256 y; }
    S[] arr;
    S single;
    mapping(uint256 => S) m;

    // storage ref ALIASES the element: writing through the ref mutates arr[0].
    // correct=42; if the AVM copied instead of aliasing → 1.
    function storageRefAlias() external returns (uint256) {
        delete arr;
        arr.push(S(1, 2));
        S storage s = arr[0];
        s.x = 42;
        return arr[0].x;
    }

    // storage→memory COPIES: mutating the memory copy must NOT change storage.
    // correct=7; if the AVM aliased → 99.
    function memoryCopyNoAlias() external returns (uint256) {
        single = S(7, 8);
        S memory mem = single;
        mem.x = 99;
        return single.x;
    }

    // memory→memory ALIASES: both names share one array.
    // correct=11; if the AVM copied → 5.
    function memoryToMemoryAlias() external pure returns (uint256) {
        uint256[] memory a = new uint256[](2);
        a[0] = 5;
        uint256[] memory b = a;
        b[0] = 11;
        return a[0];
    }

    // struct assignment into storage COPIES by value: mutate source, dest unchanged.
    // correct=3; if the AVM aliased → 100.
    function structAssignCopy() external returns (uint256) {
        S memory src = S(3, 4);
        single = src;
        src.x = 100;
        return single.x;
    }

    // array assignment (memory) COPIES element values into a fresh array.
    // correct=5 (a unchanged); if aliased → 11.
    function arrayValueCopy() external pure returns (uint256) {
        uint256[] memory a = new uint256[](1);
        a[0] = 5;
        uint256[] memory b = a;   // alias
        uint256[] memory c = new uint256[](1);
        c[0] = a[0];              // value copy of the element
        b[0] = 11;                // mutates a too (alias), but not c
        return c[0];
    }

    // storage ref into a mapping value: write through ref is visible via the mapping.
    // correct=77.
    function mappingRefWrite() external returns (uint256) {
        S storage s = m[5];
        s.x = 77;
        return m[5].x;
    }

    // delete on a storage struct zeroes every field.
    // correct=0.
    function deleteStorageStruct() external returns (uint256) {
        single = S(9, 9);
        delete single;
        return single.x + single.y;
    }

    // passing a storage array to an internal fn that takes `storage` mutates in place;
    // a `memory` param would not. correct=5.
    function storageParamMutates() external returns (uint256) {
        delete arr;
        arr.push(S(0, 0));
        _bump(arr);
        return arr[0].x;
    }
    function _bump(S[] storage a) internal { a[0].x = 5; }
}
