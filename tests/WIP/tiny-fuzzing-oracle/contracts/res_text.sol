// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
// TextResolver core: nested mapping with a STRING KEY and string value.
contract TextCore {
    mapping(bytes32 => uint64) recordVersions;
    mapping(uint64 => mapping(bytes32 => mapping(string => string))) versionable_texts;
    function setText(bytes32 node, string calldata key, string calldata value) external {
        versionable_texts[recordVersions[node]][node][key] = value;
    }
    function text(bytes32 node, string calldata key) external view returns (string memory) {
        return versionable_texts[recordVersions[node]][node][key];
    }
    function clearRecords(bytes32 node) external { recordVersions[node]++; }
}
