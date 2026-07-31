// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
contract NameCore {
    mapping(bytes32 => uint64) recordVersions;
    mapping(uint64 => mapping(bytes32 => string)) versionable_names;
    function setName(bytes32 node, string calldata newName) external {
        versionable_names[recordVersions[node]][node] = newName;
    }
    function name(bytes32 node) external view returns (string memory) {
        return versionable_names[recordVersions[node]][node];
    }
    function clearRecords(bytes32 node) external { recordVersions[node]++; }
}
