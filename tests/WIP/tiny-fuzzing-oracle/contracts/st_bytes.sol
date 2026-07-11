// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract BS {
    bytes public data;
    function pushByte(uint8 b)        external { data.push(bytes1(b)); }
    function pop()                    external { data.pop(); }            // reverts on empty
    function setByte(uint256 i, uint8 b) external { data[i] = bytes1(b); } // OOB reverts
    function len()                    external view returns (uint256) { return data.length; }
    function clear()                  external { delete data; }
    function at(uint256 i)            external view returns (uint8) { return uint8(data[i]); }  // OOB reverts
}
