// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract Vesting {
    struct Schedule { uint256 total; uint256 released; uint64 start; uint64 duration; bool revoked; }
    mapping(address => Schedule) public schedules;
    address[] public beneficiaries;

    function create(address b, uint256 total, uint64 start, uint64 duration) external {
        schedules[b] = Schedule(total, 0, start, duration, false);
        beneficiaries.push(b);
    }
    function vested(address b, uint64 ts) public view returns (uint256) {
        Schedule memory s = schedules[b];
        if (s.revoked) return s.released;
        if (ts < s.start) return 0;
        if (ts >= s.start + s.duration) return s.total;
        return (s.total * (ts - s.start)) / s.duration;
    }
    function releasable(address b, uint64 ts) public view returns (uint256) {
        return vested(b, ts) - schedules[b].released;
    }
    function release(address b, uint64 ts) external returns (uint256) {
        uint256 amt = releasable(b, ts);
        schedules[b].released += amt;
        return amt;
    }
    function revoke(address b, uint64 ts) external {
        uint256 v = vested(b, ts);
        schedules[b].released = v;
        schedules[b].revoked = true;
    }
    function count() external view returns (uint256) { return beneficiaries.length; }
}
