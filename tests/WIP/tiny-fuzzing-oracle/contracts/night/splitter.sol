// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract Splitter {
    address[] public payees;
    mapping(address => uint256) public sharesOf;
    mapping(address => uint256) public released;
    uint256 public totalShares;
    uint256 public totalReleased;
    uint256 public totalReceived;
    function addPayee(address a, uint256 sh) external { payees.push(a); sharesOf[a] += sh; totalShares += sh; }
    function receive_(uint256 amt) external { totalReceived += amt; }
    function releasable(address a) public view returns (uint256) {
        if (totalShares == 0) return 0;
        uint256 totalReceivedForA = ((totalReceived) * sharesOf[a]) / totalShares;
        return totalReceivedForA - released[a];
    }
    function release(address a) external returns (uint256 payment) {
        payment = releasable(a);
        released[a] += payment;
        totalReleased += payment;
    }
    function count() external view returns (uint256) { return payees.length; }
}
