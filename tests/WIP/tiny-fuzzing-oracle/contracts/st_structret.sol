// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { uint8 u8; int16 i16; uint64 u64; int128 i128; }
    S st;
    function rt(uint8 a, int128 d) external returns (S memory) { st.u8 = a; st.i128 = d; return st; }     // storage struct -> return (getter-like encode)
    function rtMem(uint8 a, int128 d) external pure returns (S memory) { S memory m; m.u8 = a; m.i128 = d; return m; }  // memory struct -> return
    function field(int128 d) external returns (int128) { st.i128 = d; return st.i128; }                   // control: explicit field read
}
