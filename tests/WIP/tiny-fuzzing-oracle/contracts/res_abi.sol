// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
// ABIResolver core: nested-mapping storage-ref local + bit-iteration loop +
// (uint256,bytes) multi-return.
contract ABICore {
    mapping(bytes32 => uint64) recordVersions;
    mapping(uint64 => mapping(bytes32 => mapping(uint256 => bytes))) versionable_abis;
    function setABI(bytes32 node, uint256 contentType, bytes calldata data) external {
        require(((contentType - 1) & contentType) == 0);
        versionable_abis[recordVersions[node]][node][contentType] = data;
    }
    function ABI(bytes32 node, uint256 contentTypes) external view returns (uint256, bytes memory) {
        mapping(uint256 => bytes) storage abiset = versionable_abis[recordVersions[node]][node];
        for (uint256 ct = 1; ct <= contentTypes && ct != 0; ct <<= 1) {
            if ((ct & contentTypes) != 0 && abiset[ct].length > 0) return (ct, abiset[ct]);
        }
        return (0, bytes(""));
    }
    function clearRecords(bytes32 node) external { recordVersions[node]++; }
}
