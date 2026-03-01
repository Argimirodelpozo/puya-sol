// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simple lottery using deterministic seed for testing.
 * Players enter with tickets, manager picks winner using seed.
 */
contract LotteryTest {
    address private _manager;
    uint256 private _ticketPrice;
    uint256 private _playerCount;
    uint256 private _prizePool;
    bool private _isActive;
    address private _lastWinner;
    uint256 private _roundNumber;

    mapping(uint256 => address) private _players;

    constructor() {
        _manager = msg.sender;
        _ticketPrice = 100;
        _isActive = true;
        _roundNumber = 1;
    }

    function manager() external view returns (address) {
        return _manager;
    }

    function ticketPrice() external view returns (uint256) {
        return _ticketPrice;
    }

    function playerCount() external view returns (uint256) {
        return _playerCount;
    }

    function prizePool() external view returns (uint256) {
        return _prizePool;
    }

    function isActive() external view returns (bool) {
        return _isActive;
    }

    function lastWinner() external view returns (address) {
        return _lastWinner;
    }

    function roundNumber() external view returns (uint256) {
        return _roundNumber;
    }

    function setTicketPrice(uint256 price) external {
        require(msg.sender == _manager, "Not manager");
        require(_playerCount == 0, "Round in progress");
        _ticketPrice = price;
    }

    function enter(address player) external {
        require(_isActive, "Lottery not active");

        _playerCount += 1;
        _players[_playerCount] = player;
        _prizePool += _ticketPrice;
    }

    function getPlayer(uint256 index) external view returns (address) {
        return _players[index];
    }

    function pickWinner(uint256 seed) external returns (address) {
        require(msg.sender == _manager, "Not manager");
        require(_playerCount > 0, "No players");

        uint256 winnerIndex = (seed % _playerCount) + 1;
        address winner = _players[winnerIndex];

        _lastWinner = winner;
        _prizePool = 0;
        _playerCount = 0;
        _roundNumber += 1;

        return winner;
    }

    function toggleActive() external {
        require(msg.sender == _manager, "Not manager");
        _isActive = !_isActive;
    }
}
