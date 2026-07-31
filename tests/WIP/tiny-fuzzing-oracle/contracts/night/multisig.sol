// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract MultiSig {
    address[] public owners;
    mapping(address => bool) public isOwner;
    uint256 public required;
    struct Tx { address to; uint256 value; bool executed; uint256 confirmations; }
    Tx[] public txs;
    mapping(uint256 => mapping(address => bool)) public confirmed;
    function addOwner(address o) external { if (!isOwner[o]) { isOwner[o] = true; owners.push(o); } }
    function setRequired(uint256 r) external { required = r; }
    function submit(address to, uint256 value) external returns (uint256 id) {
        id = txs.length; txs.push(Tx(to, value, false, 0));
    }
    function confirm(uint256 id, address by) external {
        require(isOwner[by], "not owner");
        require(!confirmed[id][by], "dup");
        confirmed[id][by] = true; txs[id].confirmations += 1;
    }
    function revoke(uint256 id, address by) external {
        require(confirmed[id][by], "not confirmed");
        confirmed[id][by] = false; txs[id].confirmations -= 1;
    }
    function execute(uint256 id) external returns (bool) {
        Tx storage t = txs[id];
        require(!t.executed, "done");
        require(t.confirmations >= required, "insufficient");
        t.executed = true;
        return true;
    }
    function ownerCount() external view returns (uint256) { return owners.length; }
}
