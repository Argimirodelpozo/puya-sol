// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Storage-semantics parity matrix vs solc, self-verifying under
// --evm-storage-layout: every probe asserts raw sload() words against the
// values solc's layout/derivation rules pin (StorageOffsets::computeOffsets,
// mapping slot = keccak(encodedKey ++ slot32), dynamic data at keccak(slot),
// short/long string form, packed byte positions from the LSB).
contract StorageParitySlots {
    uint8 public a = 0xAA;       // slot 0, offset 0
    uint16 public b = 0xBBCC;    // slot 0, offset 1
    bool public c = true;        // slot 0, offset 3
    mapping(string => uint256) public m;   // slot 1
    mapping(int16 => uint256) public mi;   // slot 2
    uint8[] public arr8;                   // slot 3
    string public s;                       // slot 4

    function packedWord() public view returns (bool ok, uint256 w) {
        assembly { w := sload(0) }
        // c<<24 | b<<8 | a
        ok = (w == 0x01BBCCAA);
    }

    function packedWrite() public returns (bool ok, uint256 w) {
        b = 0x1122;
        assembly { w := sload(0) }
        ok = (w == 0x011122AA) && (a == 0xAA) && c;
        b = 0xBBCC; // restore
    }

    function stringKey() public returns (bool ok) {
        m["hi"] = 7;
        // raw bytes ++ slot32 (string keys are NOT padded or hashed first)
        bytes32 slot = keccak256(abi.encodePacked("hi", uint256(1)));
        uint256 v;
        assembly { v := sload(slot) }
        ok = (v == 7);
    }

    function signedKey() public returns (bool ok) {
        mi[-2] = 9;
        // signed keys sign-extend to the full 256-bit word before hashing
        bytes32 slot = keccak256(abi.encodePacked(int256(-2), uint256(2)));
        uint256 v;
        assembly { v := sload(slot) }
        ok = (v == 9);
    }

    function packedArray() public returns (bool ok, uint256 len, uint256 w) {
        arr8.push(1);
        arr8.push(2);
        arr8.push(3);
        assembly { len := sload(3) }
        bytes32 base = keccak256(abi.encodePacked(uint256(3)));
        assembly { w := sload(base) }
        ok = (len == 3) && (w == 0x030201);
    }

    function popZeroes() public returns (bool ok, uint256 len, uint256 w) {
        // continues from packedArray state [1,2,3]
        arr8.pop();
        assembly { len := sload(3) }
        bytes32 base = keccak256(abi.encodePacked(uint256(3)));
        assembly { w := sload(base) }
        // pop must ZERO the vacated byte, not just the length
        ok = (len == 2) && (w == 0x0201);
    }

    function shortString() public returns (bool ok, uint256 w) {
        s = "abc";
        assembly { w := sload(4) }
        // short form: data left-aligned ++ zeros ++ (len*2) in the low byte
        ok = (w ==
            0x6162630000000000000000000000000000000000000000000000000000000006);
    }

    function longString() public returns (bool ok, uint256 w, uint256 d0) {
        s = "0123456789012345678901234567890123456789"; // 40 chars
        assembly { w := sload(4) }
        bytes32 base = keccak256(abi.encodePacked(uint256(4)));
        assembly { d0 := sload(base) }
        // long form: slot holds len*2+1; data at keccak(slot)
        ok = (w == 81)
            && (d0 ==
            0x3031323334353637383930313233343536373839303132333435363738393031);
    }

    int16[] public si;     // slot 5 — packed 16 elems/slot, two's complement

    function signedArrayWord() public returns (bool ok, uint256 w) {
        si.push(-1);
        si.push(2);
        bytes32 base = keccak256(abi.encodePacked(uint256(5)));
        assembly { w := sload(base) }
        // elem0 two's-complement 0xFFFF at the LSB, elem1 0x0002 above it
        ok = (w == 0x0002FFFF) && (si[0] == -1) && (si[1] == 2);
    }

    function slotOffsetFacts() public view returns (bool ok) {
        uint256 sl;
        uint256 off;
        assembly {
            sl := b.slot
            off := b.offset
        }
        // b: slot 0, byte offset 1 (after uint8 a)
        ok = (sl == 0) && (off == 1);
    }

    function shortLongRoundTrip() public returns (bool ok, uint256 w) {
        s = "0123456789012345678901234567890123456789"; // long (40)
        s = "xy";                                        // back to short
        assembly { w := sload(4) }
        ok = (w ==
            0x7879000000000000000000000000000000000000000000000000000000000004)
            && (bytes(s).length == 2);
    }
}

// Functional twin — runs in BOTH storage modes (no asm slot asserts).
contract StorageParityCore {
    struct S {
        uint256 x;
        mapping(uint256 => uint256) mm;
    }
    S internal st;
    uint256[] internal arr;
    uint256[] internal arr2;

    function deleteStructKeepsMapping() public returns (bool ok) {
        st.x = 5;
        st.mm[1] = 6;
        delete st;
        // solc: delete zeroes value members, mappings are UNTOUCHED
        ok = (st.x == 0) && (st.mm[1] == 6);
    }

    function deleteArrayZeroesElements() public returns (bool ok) {
        arr.push(11);
        arr.push(22);
        delete arr;
        // reading past length reverts; re-push and check the slot was zeroed
        arr.push(0);
        ok = (arr.length == 1) && (arr[0] == 0);
    }

    function pushRefAndDefault() public returns (bool ok) {
        while (arr2.length > 0) arr2.pop();
        arr2.push() = 7;      // push() returns a reference
        arr2.push();          // bare push appends zero
        ok = (arr2.length == 2) && (arr2[0] == 7) && (arr2[1] == 0);
    }

    function popThenPushReadsZero() public returns (bool ok) {
        while (arr2.length > 0) arr2.pop();
        arr2.push(99);
        arr2.pop();
        arr2.push();          // must observe a ZEROED slot, not stale 99
        ok = (arr2[0] == 0);
    }

    struct P {
        uint64 u;
        uint256 v;
    }
    P internal p1;
    P internal p2;
    uint256[] internal src;
    uint256[] internal dst;

    function structStorageCopy() public returns (bool ok) {
        p1.u = 3;
        p1.v = 4;
        p2 = p1;              // storage->storage: element-wise DEEP copy
        p1.u = 9;             // independence after the copy
        ok = (p2.u == 3) && (p2.v == 4) && (p1.u == 9);
    }

    function arrayStorageCopy() public returns (bool ok) {
        while (src.length > 0) src.pop();
        while (dst.length > 0) dst.pop();
        src.push(1);
        src.push(2);
        dst.push(77);
        dst.push(88);
        dst.push(99);
        dst = src;            // resizes down AND copies element-wise
        src.push(3);
        ok = (dst.length == 2) && (dst[0] == 1) && (dst[1] == 2)
            && (src.length == 3);
    }
}
