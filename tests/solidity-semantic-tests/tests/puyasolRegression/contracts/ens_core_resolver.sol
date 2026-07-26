// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
// CUSTOM regression fixture (NOT vendored) — the five simple ENS resolver
// profiles (Addr/Text/ContentHash/Name/Pubkey) combined, mirroring
// PublicResolver. Guards that their nested-mapping storage coexists without
// box-key aliasing across profiles, string mapping KEYS work (Text), and the
// AddrResolver asm addr<->bytes (exp fold + memory-pointer round-trip) works in
// the aggregate. Differential-verified (65 sequenced calls vs live solc+EVM).
// See ens-compile / memory-pointer-seam / asm-exp-constant-fold.
contract CoreResolverCore {
    uint256 constant COIN_TYPE_ETH = 60;
    mapping(bytes32 => uint64) recordVersions;
    mapping(uint64 => mapping(bytes32 => mapping(uint256 => bytes))) versionable_addresses;
    mapping(uint64 => mapping(bytes32 => mapping(string => string))) versionable_texts;
    mapping(uint64 => mapping(bytes32 => bytes)) versionable_hashes;
    mapping(uint64 => mapping(bytes32 => string)) versionable_names;
    struct PublicKey { bytes32 x; bytes32 y; }
    mapping(uint64 => mapping(bytes32 => PublicKey)) versionable_pubkeys;

    function setAddr(bytes32 node, uint256 ct, bytes calldata a) external {
        versionable_addresses[recordVersions[node]][node][ct] = a;
    }
    function addr(bytes32 node, uint256 ct) external view returns (bytes memory) {
        return versionable_addresses[recordVersions[node]][node][ct];
    }
    function setAddrEth(bytes32 node, address a) external {
        versionable_addresses[recordVersions[node]][node][COIN_TYPE_ETH] = addressToBytes(a);
    }
    function addrEth(bytes32 node) external view returns (address payable) {
        bytes memory a = versionable_addresses[recordVersions[node]][node][COIN_TYPE_ETH];
        if (a.length == 0) return payable(0);
        return bytesToAddress(a);
    }
    function setText(bytes32 node, string calldata k, string calldata v) external {
        versionable_texts[recordVersions[node]][node][k] = v;
    }
    function text(bytes32 node, string calldata k) external view returns (string memory) {
        return versionable_texts[recordVersions[node]][node][k];
    }
    function setContenthash(bytes32 node, bytes calldata h) external {
        versionable_hashes[recordVersions[node]][node] = h;
    }
    function contenthash(bytes32 node) external view returns (bytes memory) {
        return versionable_hashes[recordVersions[node]][node];
    }
    function setName(bytes32 node, string calldata n) external {
        versionable_names[recordVersions[node]][node] = n;
    }
    function getName(bytes32 node) external view returns (string memory) {
        return versionable_names[recordVersions[node]][node];
    }
    function setPubkey(bytes32 node, bytes32 x, bytes32 y) external {
        versionable_pubkeys[recordVersions[node]][node] = PublicKey(x, y);
    }
    function pubkey(bytes32 node) external view returns (bytes32, bytes32) {
        uint64 ver = recordVersions[node];
        return (versionable_pubkeys[ver][node].x, versionable_pubkeys[ver][node].y);
    }
    function clearRecords(bytes32 node) external { recordVersions[node]++; }

    function bytesToAddress(bytes memory b) internal pure returns (address payable a) {
        require(b.length == 20);
        assembly { a := div(mload(add(b, 32)), exp(256, 12)) }
    }
    function addressToBytes(address a) internal pure returns (bytes memory b) {
        b = new bytes(20);
        assembly { mstore(add(b, 32), mul(a, exp(256, 12))) }
    }
}
