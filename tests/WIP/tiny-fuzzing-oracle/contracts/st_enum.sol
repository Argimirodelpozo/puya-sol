// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract ES {
    enum E { A, B, C, D }
    E public state;
    mapping(uint256 => E) public m;
    function setState(uint8 v)        external { state = E(v); }        // reverts if v >= 4
    function setM(uint256 k, uint8 v) external { m[k] = E(v); }
    function advance()                external { state = E((uint8(state) + 1) % 4); }
    function asUint()                 external view returns (uint256) { return uint256(state); }
}
