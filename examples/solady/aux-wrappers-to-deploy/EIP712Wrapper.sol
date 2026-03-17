// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/EIP712.sol";

contract EIP712Wrapper is EIP712 {
    function getDomainSeparator() external view returns (bytes32) {
        return _domainSeparator();
    }

    function _domainNameAndVersion() internal pure override returns (string memory name, string memory version) {
        name = "TestDomain";
        version = "1";
    }
}
