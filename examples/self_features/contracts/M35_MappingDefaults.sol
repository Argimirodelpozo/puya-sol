// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M35: Mapping default values (Gap 7 verification).
 * Tests that reading unset mapping keys returns Solidity defaults (zero values)
 * instead of crashing, matching EVM behavior.
 */

struct Info {
    uint256 amount;
    bool active;
}

contract MappingDefaultsTest {
    mapping(uint256 => uint256) private _values;
    mapping(uint256 => bool) private _flags;
    mapping(uint256 => Info) private _infos;
    mapping(address => uint256) private _balances;
    mapping(uint256 => mapping(uint256 => uint256)) private _nested;

    // ── Read uninitialized uint256 ──
    function getValue(uint256 key) external view returns (uint256) {
        return _values[key];
    }

    // ── Read uninitialized bool ──
    function getFlag(uint256 key) external view returns (bool) {
        return _flags[key];
    }

    // ── Read uninitialized struct ──
    function getInfo(uint256 key) external view returns (uint256, bool) {
        return (_infos[key].amount, _infos[key].active);
    }

    // ── Read uninitialized address-keyed mapping ──
    function getBalance(address who) external view returns (uint256) {
        return _balances[who];
    }

    // ── Read uninitialized nested mapping ──
    function getNested(uint256 a, uint256 b) external view returns (uint256) {
        return _nested[a][b];
    }

    // ── Write then read different key (verifies isolation) ──
    function setValue(uint256 key, uint256 val) external {
        _values[key] = val;
    }

    function setFlag(uint256 key, bool val) external {
        _flags[key] = val;
    }

    function setInfo(uint256 key, uint256 amount, bool active) external {
        _infos[key] = Info(amount, active);
    }

    function setBalance(address who, uint256 val) external {
        _balances[who] = val;
    }

    function setNested(uint256 a, uint256 b, uint256 val) external {
        _nested[a][b] = val;
    }

    // ── Conditional on default value ──
    function isZero(uint256 key) external view returns (bool) {
        return _values[key] == 0;
    }

    // ── Increment from default (should start at 0) ──
    function increment(uint256 key) external {
        _values[key] += 1;
    }
}
