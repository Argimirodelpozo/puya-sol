// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simple English auction contract.
 * Tracks bids using mappings, allows bidding, withdrawing, and ending the auction.
 */
contract AuctionTest {
    address private _owner;
    address private _highestBidder;
    uint256 private _highestBid;
    bool private _ended;
    uint256 private _endTime;

    mapping(address => uint256) private _pendingReturns;

    constructor() {
        _owner = msg.sender;
        _endTime = 1000000; // Far future block for testing
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function highestBidder() external view returns (address) {
        return _highestBidder;
    }

    function highestBid() external view returns (uint256) {
        return _highestBid;
    }

    function ended() external view returns (bool) {
        return _ended;
    }

    function bid(address bidder, uint256 amount) external {
        require(!_ended, "Auction ended");
        require(amount > _highestBid, "Bid not high enough");

        if (_highestBid != 0) {
            _pendingReturns[_highestBidder] += _highestBid;
        }

        _highestBidder = bidder;
        _highestBid = amount;
    }

    function pendingReturn(address bidder) external view returns (uint256) {
        return _pendingReturns[bidder];
    }

    function withdraw(address bidder) external returns (uint256) {
        uint256 amount = _pendingReturns[bidder];
        if (amount > 0) {
            _pendingReturns[bidder] = 0;
        }
        return amount;
    }

    function endAuction() external {
        require(msg.sender == _owner, "Not owner");
        require(!_ended, "Already ended");
        _ended = true;
    }
}
