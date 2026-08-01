// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
import "./profiles/DNSResolver.sol";
contract TestDNSResolver is DNSResolver {
    function isAuthorised(bytes32) internal view override returns (bool) { return true; }
}
