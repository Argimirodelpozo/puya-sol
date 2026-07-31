// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract PackedInts {
    uint8[] u8; uint16[] u16; int8[] i8; bytes1[] b1;
    function u8PushRead(uint8 a, uint8 b, uint8 c) external returns (uint8,uint8,uint8) {
        delete u8; u8.push(a); u8.push(b); u8.push(c); return (u8[0],u8[1],u8[2]);
    }
    function u8Swap(uint8 a, uint8 b) external returns (uint8,uint8) {
        delete u8; u8.push(a); u8.push(b); (u8[0],u8[1])=(u8[1],u8[0]); return (u8[0],u8[1]);
    }
    function u16PushRead(uint16 a, uint16 b, uint16 c) external returns (uint16,uint16,uint16) {
        delete u16; u16.push(a); u16.push(b); u16.push(c); return (u16[0],u16[1],u16[2]);
    }
    function i8PushRead(int8 a, int8 b, int8 c) external returns (int8,int8,int8) {
        delete i8; i8.push(a); i8.push(b); i8.push(c); return (i8[0],i8[1],i8[2]);
    }
    function b1PushRead(bytes1 a, bytes1 b) external returns (bytes1,bytes1) {
        delete b1; b1.push(a); b1.push(b); return (b1[0],b1[1]);
    }
}
