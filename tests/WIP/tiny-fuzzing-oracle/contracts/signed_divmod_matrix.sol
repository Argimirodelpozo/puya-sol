// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function cI8d(int8 a,int8 b) external pure returns (int8){int8 x=a; x/=b; return x;}
    function cI8m(int8 a,int8 b) external pure returns (int8){int8 x=a; x%=b; return x;}
    function cI16d(int16 a,int16 b) external pure returns (int16){int16 x=a; x/=b; return x;}
    function cI32d(int32 a,int32 b) external pure returns (int32){int32 x=a; x/=b; return x;}
    function cI64d(int64 a,int64 b) external pure returns (int64){int64 x=a; x/=b; return x;}
    function cI64m(int64 a,int64 b) external pure returns (int64){int64 x=a; x%=b; return x;}
    function ucI8d(int8 a,int8 b) external pure returns (int8){int8 x=a; unchecked{x/=b;} return x;}
    function cI128d(int128 a,int128 b) external pure returns (int128){int128 x=a; x/=b; return x;}
    function mixI64by8(int64 a,int8 b) external pure returns (int64){int64 x=a; x/=int64(b); return x;}
    function cU8d(uint8 a,uint8 b) external pure returns (uint8){uint8 x=a; x/=b; return x;}
}
