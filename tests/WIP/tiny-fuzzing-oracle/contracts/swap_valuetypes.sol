// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract SwapValueTypes {
    uint256[] su; int128[] si; address[] sa; bytes32[] sb;
    // storage swaps across value types
    function suSwap(uint256 a, uint256 b) external returns (uint256,uint256){ delete su; su.push(a); su.push(b); (su[0],su[1])=(su[1],su[0]); return (su[0],su[1]); }
    function siSwap(int128 a, int128 b) external returns (int128,int128){ delete si; si.push(a); si.push(b); (si[0],si[1])=(si[1],si[0]); return (si[0],si[1]); }
    function saSwap(address a, address b) external returns (address,address){ delete sa; sa.push(a); sa.push(b); (sa[0],sa[1])=(sa[1],sa[0]); return (sa[0],sa[1]); }
    function sbSwap(bytes32 a, bytes32 b) external returns (bytes32,bytes32){ delete sb; sb.push(a); sb.push(b); (sb[0],sb[1])=(sb[1],sb[0]); return (sb[0],sb[1]); }
    // memory swaps
    function muSwap(uint256 a, uint256 b) external pure returns (uint256,uint256){ uint256[] memory m=new uint256[](2); m[0]=a;m[1]=b; (m[0],m[1])=(m[1],m[0]); return (m[0],m[1]); }
    function miSwap(int128 a, int128 b) external pure returns (int128,int128){ int128[] memory m=new int128[](2); m[0]=a;m[1]=b; (m[0],m[1])=(m[1],m[0]); return (m[0],m[1]); }
    function mbool(bool a, bool b) external pure returns (bool,bool){ bool[] memory m=new bool[](2); m[0]=a;m[1]=b; (m[0],m[1])=(m[1],m[0]); return (m[0],m[1]); }
    // storage 3-way rotate + mixed local
    function suRot3(uint256 a,uint256 b,uint256 c) external returns(uint256,uint256,uint256){ delete su; su.push(a);su.push(b);su.push(c); (su[0],su[1],su[2])=(su[2],su[0],su[1]); return (su[0],su[1],su[2]); }
    function mixLocal(uint256 a,uint256 x) external returns(uint256,uint256){ delete su; su.push(a); (su[0],x)=(x,su[0]); return (su[0],x); }
}
