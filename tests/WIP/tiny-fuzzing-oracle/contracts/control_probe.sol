// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Control flow, recursion, array bounds (OOB reverts), loop overflow.
contract Control {
    uint256[] arr;
    function oobRead(uint256 i) external returns (uint256) { delete arr; arr.push(5); arr.push(6); return arr[i]; } // i>=2 reverts
    function loopSum(uint256 n) external pure returns (uint256) { uint256 s=0; for (uint256 k=0;k<(n%50);k++){ s+=k; } return s; }
    function factorial(uint8 n) external pure returns (uint256) { uint256 p=1; for (uint8 k=1;k<=(n%12);k++){ p*=k; } return p; }
    function fib(uint256 n) external pure returns (uint256) { return _fib(n%18); }
    function _fib(uint256 n) internal pure returns (uint256) { return n<2 ? n : _fib(n-1)+_fib(n-2); }
    function nested(uint8 a, uint8 b) external pure returns (uint256) { uint256 s=0; for(uint8 i=0;i<(a%12);i++) for(uint8 j=0;j<(b%12);j++) s+=1; return s; }
    function breakCont(uint256 n) external pure returns (uint256) { uint256 s=0; for(uint256 k=0;k<(n%30);k++){ if(k%2==0) continue; if(k>20) break; s+=k; } return s; }
    function whileLoop(uint256 n) external pure returns (uint256) { uint256 k=n%40; uint256 c=0; while(k>0){ k=k/2; c++; } return c; }
}
