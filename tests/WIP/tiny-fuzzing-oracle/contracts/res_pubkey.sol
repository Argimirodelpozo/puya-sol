// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
contract PubkeyCore {
    struct PublicKey { bytes32 x; bytes32 y; }
    mapping(bytes32 => uint64) recordVersions;
    mapping(uint64 => mapping(bytes32 => PublicKey)) versionable_pubkeys;
    function setPubkey(bytes32 node, bytes32 x, bytes32 y) external {
        versionable_pubkeys[recordVersions[node]][node] = PublicKey(x, y);
    }
    function pubkey(bytes32 node) external view returns (bytes32 x, bytes32 y) {
        uint64 v = recordVersions[node];
        return (versionable_pubkeys[v][node].x, versionable_pubkeys[v][node].y);
    }
    function clearRecords(bytes32 node) external { recordVersions[node]++; }
}
