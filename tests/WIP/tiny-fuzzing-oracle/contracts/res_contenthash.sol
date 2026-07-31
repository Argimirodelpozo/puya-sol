// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
contract ContentHashCore {
    event ContenthashChanged(bytes32 indexed node, bytes hash);
    mapping(bytes32 => uint64) recordVersions;
    mapping(uint64 => mapping(bytes32 => bytes)) versionable_hashes;
    function setContenthash(bytes32 node, bytes calldata hash) external {
        versionable_hashes[recordVersions[node]][node] = hash;
        emit ContenthashChanged(node, hash);
    }
    function contenthash(bytes32 node) external view returns (bytes memory) {
        return versionable_hashes[recordVersions[node]][node];
    }
    function clearRecords(bytes32 node) external { recordVersions[node]++; }
}
