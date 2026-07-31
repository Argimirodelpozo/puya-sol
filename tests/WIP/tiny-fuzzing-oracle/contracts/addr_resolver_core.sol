// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;

// Self-contained differential fixture mirroring ENS AddrResolver's core: the
// versioned nested-mapping bytes storage + the asm addr<->bytes conversions
// (exp(256,12) = 2^96 constant-fold) + events. Open (no ERC165/auth) so the
// differential exercises the resolver logic + the exp asm path.
contract AddrResolverCore {
    event AddrChanged(bytes32 indexed node, address a);
    event AddressChanged(bytes32 indexed node, uint256 coinType, bytes newAddress);

    uint256 private constant COIN_TYPE_ETH = 60;
    mapping(bytes32 => uint64) public recordVersions;
    mapping(uint64 => mapping(bytes32 => mapping(uint256 => bytes))) versionable_addresses;

    function setAddrEth(bytes32 node, address a) external {
        setAddr(node, COIN_TYPE_ETH, addressToBytes(a));
    }
    function addrEth(bytes32 node) public view returns (address payable) {
        bytes memory a = addr(node, COIN_TYPE_ETH);
        if (a.length == 0) return payable(0);
        return bytesToAddress(a);
    }
    function setAddr(bytes32 node, uint256 coinType, bytes memory a) public {
        emit AddressChanged(node, coinType, a);
        if (coinType == COIN_TYPE_ETH) emit AddrChanged(node, bytesToAddress(a));
        versionable_addresses[recordVersions[node]][node][coinType] = a;
    }
    function addr(bytes32 node, uint256 coinType) public view returns (bytes memory) {
        return versionable_addresses[recordVersions[node]][node][coinType];
    }
    function clearRecords(bytes32 node) public { recordVersions[node]++; }

    function bytesToAddress(bytes memory b) internal pure returns (address payable a) {
        require(b.length == 20);
        assembly { a := div(mload(add(b, 32)), exp(256, 12)) }
    }
    function addressToBytes(address a) internal pure returns (bytes memory b) {
        b = new bytes(20);
        assembly { mstore(add(b, 32), mul(a, exp(256, 12))) }
    }
}
