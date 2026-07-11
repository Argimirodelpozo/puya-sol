// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    bytes  public data;                      // dynamic bytes in a box + getter
    string public name;                      // string storage + getter
    bytes32 public h;                         // fixed bytes storage
    mapping(uint256 => bytes) public chunk;   // mapping(uint=>bytes) getter
    function setData(bytes calldata b)  external { data = b; }
    function appendData(bytes calldata b) external { data = bytes.concat(data, b); }  // grow in storage
    function setName(string calldata s) external { name = s; }
    function setH(bytes32 x)            external { h = x; }
    function setChunk(uint256 k, bytes calldata b) external { chunk[k] = b; }
    function dlen()                    external view returns (uint256) { return data.length; }
}
