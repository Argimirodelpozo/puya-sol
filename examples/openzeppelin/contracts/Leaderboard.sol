// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Leaderboard {
    address private _admin;
    uint256 private _playerCount;
    uint256 private _highScore;
    uint256 private _totalGamesPlayed;

    mapping(uint256 => uint256) internal _playerHash;
    mapping(uint256 => uint256) internal _playerScore;

    constructor() {
        _admin = msg.sender;
        _playerCount = 0;
        _highScore = 0;
        _totalGamesPlayed = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getPlayerCount() external view returns (uint256) {
        return _playerCount;
    }

    function getHighScore() external view returns (uint256) {
        return _highScore;
    }

    function getTotalGamesPlayed() external view returns (uint256) {
        return _totalGamesPlayed;
    }

    function getPlayerHash(uint256 playerId) external view returns (uint256) {
        return _playerHash[playerId];
    }

    function getPlayerScore(uint256 playerId) external view returns (uint256) {
        return _playerScore[playerId];
    }

    function registerPlayer(uint256 playerHash) external returns (uint256) {
        uint256 id = _playerCount;
        _playerHash[id] = playerHash;
        _playerScore[id] = 0;
        _playerCount = id + 1;
        return id;
    }

    function updateScore(uint256 playerId, uint256 newScore) external {
        _playerScore[playerId] = newScore;
        _totalGamesPlayed = _totalGamesPlayed + 1;
        if (newScore > _highScore) {
            _highScore = newScore;
        }
    }
}

contract LeaderboardTest is Leaderboard {
    constructor() Leaderboard() {}

    function initPlayer(uint256 playerId) external {
        _playerHash[playerId] = 0;
        _playerScore[playerId] = 0;
    }
}
