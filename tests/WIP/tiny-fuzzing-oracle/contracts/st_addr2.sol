// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct Acct { address who; uint256 bal; int128 d; }
    Acct public acct;                                       // struct getter with an address field
    mapping(uint256 => address) public idx;                 // mapping(uint=>address) getter
    function setAcct(address w, uint256 b, int128 d) external { acct = Acct(w, b, d); }
    function setIdx(uint256 k, address a)            external { idx[k] = a; }
}
