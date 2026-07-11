// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Stateful: storage persists across calls. Tests checked add/sub overflow + mapping + signed
// accumulation ACROSS a transaction sequence (not within one call).
contract Counter {
    uint256 public count;
    mapping(uint256 => uint256) public m;
    int128  public signedAcc;
    uint64  public packedA;
    uint64  public packedB;
    function inc(uint256 by)        external { count += by; }          // checked add across calls
    function dec(uint256 by)        external { count -= by; }          // checked sub across calls
    function setM(uint256 k, uint256 v) external { m[k] = v; }         // mapping persistence
    function addM(uint256 k, uint256 v) external { m[k] += v; }        // mapping read-modify-write
    function addSigned(int128 d)    external { signedAcc += d; }       // signed accumulate (sub-word)
    function setPacked(uint64 a, uint64 b) external { packedA = a; packedB = b; }  // packed neighbours
    function reset()                external { count = 0; }
}
