// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { uint256 a; string s; uint16 b; }
    // nested dynamic array round-trip
    function rtNested() external pure returns (uint256, uint256, uint256) {
        uint256[][] memory x = new uint256[][](2);
        x[0] = new uint256[](1); x[0][0] = 11;
        x[1] = new uint256[](2); x[1][0] = 22; x[1][1] = 33;
        bytes memory e = abi.encode(x);
        uint256[][] memory y = abi.decode(e, (uint256[][]));
        return (y[0][0], y[1][0], y[1][1]);   // (11,22,33)
    }
    // struct with string round-trip
    function rtStruct() external pure returns (uint256, string memory, uint16) {
        S memory s = S(42, "hi there", 7);
        bytes memory e = abi.encode(s);
        S memory t = abi.decode(e, (S));
        return (t.a, t.s, t.b);   // (42, "hi there", 7)
    }
    // mixed static/dynamic tuple round-trip
    function rtTuple() external pure returns (uint8, bytes memory, uint16, bool) {
        bytes memory e = abi.encode(uint8(9), bytes(hex"deadbeef"), uint16(513), true);
        (uint8 a, bytes memory b, uint16 c, bool d) =
            abi.decode(e, (uint8, bytes, uint16, bool));
        return (a, b, c, d);   // (9, 0xdeadbeef, 513, true)
    }
    struct T { uint256[] arr; bytes raw; }
    // struct with uint256[] and bytes fields round-trip
    function rtStructArr() external pure returns (uint256, uint256, uint256, uint256) {
        uint256[] memory a = new uint256[](2); a[0] = 5; a[1] = 6;
        T memory t = T(a, hex"a1b2c3");
        bytes memory e = abi.encode(t);
        T memory u = abi.decode(e, (T));
        return (u.arr[0], u.arr[1], u.arr.length, u.raw.length);   // (5,6,2,3)
    }
    // bytes32 + address round-trip
    function rtFixed() external view returns (bytes32, address) {
        bytes32 h = keccak256("x");
        bytes memory e = abi.encode(h, address(this));
        (bytes32 h2, address a2) = abi.decode(e, (bytes32, address));
        return (h2, a2);
    }
}
