// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract NativeValueSelfdestruct {
    function close(address payable beneficiary) external {
        selfdestruct(beneficiary);
    }
}
