// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/// Storage shapes that drive the --evm-storage-layout dispatcher's packed
/// element loop (EvmSlotStorageDispatch): the loop reads one storage word per
/// `__per` elements and slices each element out of it, so an off-by-one in the
/// word boundary, the lane offset, or the element width aliases neighbours
/// instead of failing loudly.
///
/// Shapes chosen deliberately:
///   * nested mapping of address — the shape a real CCTP integration uses,
///     and address is the awkward width (20 bytes stored, 32 encoded)
///   * uint32[] — 8 elements per 32-byte word, so a whole-array read crosses
///     word boundaries repeatedly
///   * address[] — 1 per word after packing rules, the width above
///   * uint128[] — exactly 2 per word, the boundary case
contract SlotPackedDispatch {
    mapping(uint32 => mapping(address => address)) public peers;
    mapping(address => address) public simple;

    uint32[] public smalls;
    address[] public addrs;
    uint128[] public halves;

    function setPeer(uint32 d, address k, address v) public { peers[d][k] = v; }
    function setSimple(address k, address v) public { simple[k] = v; }

    function peerOf(uint32 d, address k) public view returns (address) {
        return peers[d][k];
    }

    function pushSmall(uint32 v) public { smalls.push(v); }
    function pushAddr(address v) public { addrs.push(v); }
    function pushHalf(uint128 v) public { halves.push(v); }

    // Whole-aggregate reads: these are what run the packed multi-lane loop.
    function allSmalls() public view returns (uint32[] memory) { return smalls; }
    function allAddrs() public view returns (address[] memory) { return addrs; }
    function allHalves() public view returns (uint128[] memory) { return halves; }

    function smallsLen() public view returns (uint256) { return smalls.length; }

    // Element-wise reads must agree with the whole-aggregate read above.
    function smallAt(uint256 i) public view returns (uint32) { return smalls[i]; }
    function addrAt(uint256 i) public view returns (address) { return addrs[i]; }
    function halfAt(uint256 i) public view returns (uint128) { return halves[i]; }

    // Overwrite a mid-word lane: the word is rebuilt from zero at the word
    // boundary, so a neighbour clobbered here shows up as a changed sibling.
    function setSmallAt(uint256 i, uint32 v) public { smalls[i] = v; }
}
