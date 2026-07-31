// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract ERC721A {
    struct TokenOwnership { address addr; uint64 startTimestamp; bool burned; }
    struct AddressData { uint64 balance; uint64 numberMinted; uint64 numberBurned; }
    uint256 public currentIndex;
    mapping(uint256 => TokenOwnership) internal _ownerships;
    mapping(address => AddressData) internal _addressData;
    function mint(address to, uint256 qty, uint64 ts) external {
        uint256 start = currentIndex;
        _addressData[to].balance += uint64(qty);
        _addressData[to].numberMinted += uint64(qty);
        _ownerships[start] = TokenOwnership(to, ts, false);   // only first of batch set (ERC721A trick)
        currentIndex = start + qty;
    }
    function ownerOf(uint256 tokenId) public view returns (address) {
        require(tokenId < currentIndex, "nonexistent");
        uint256 cur = tokenId;
        while (true) {
            TokenOwnership memory o = _ownerships[cur];
            if (o.addr != address(0) && !o.burned) return o.addr;
            if (cur == 0) break;
            cur--;
        }
        revert("not found");
    }
    function balanceOf(address a) external view returns (uint256) { return _addressData[a].balance; }
    function burn(uint256 tokenId, uint64 ts) external {
        address owner = ownerOf(tokenId);
        _addressData[owner].balance -= 1;
        _addressData[owner].numberBurned += 1;
        _ownerships[tokenId] = TokenOwnership(owner, ts, true);
    }
    function minted(address a) external view returns (uint64) { return _addressData[a].numberMinted; }
}
