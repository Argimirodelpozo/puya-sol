// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract Enums {
    enum Status { Pending, Active, Closed }

    Status public currentStatus;

    function setStatus(Status s) external { currentStatus = s; }
    function getStatus() external view returns (Status) { return currentStatus; }
    function isPending() external view returns (bool) { return currentStatus == Status.Pending; }
    function isActive() external view returns (bool) { return currentStatus == Status.Active; }
    function statusToUint() external view returns (uint8) { return uint8(currentStatus); }
}
