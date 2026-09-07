// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// ARC-56 must describe mapping roots: key type, value type (named struct in
// `structs`), and the root prefix — holder format 2 keeps entry names as
// tagged SHA-256 derivations, which the description states.
contract Arc56MappingRoots {
    struct P2 { uint128 a; uint128 b; }
    struct Q { uint64 n; address who; }
    mapping(address => P2) public plain;
    mapping(uint256 => mapping(address => Q)) internal nested;
    mapping(string => uint256) public named;
    P2[] public arr;

    function setPlain(address k, uint128 a, uint128 b) external { plain[k] = P2(a, b); }
    function setNested(uint256 i, address k, uint64 n) external { nested[i][k] = Q(n, k); }
    function getNested(uint256 i, address k) external view returns (uint64, address) { Q storage q = nested[i][k]; return (q.n, q.who); }
    function setNamed(string calldata s, uint256 v) external { named[s] = v; }
    function pushArr(uint128 a, uint128 b) external { arr.push(P2(a, b)); }
}
