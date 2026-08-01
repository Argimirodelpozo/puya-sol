// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;

import "./profiles/AddrResolver.sol";

// Minimal concrete AddrResolver for differential testing. Open authorisation
// (isAuthorised = true) so the differential exercises the resolver logic
// (nested-mapping storage, events, asm addr<->bytes) rather than auth gating.
contract TestAddrResolver is AddrResolver {
    function isAuthorised(bytes32) internal view override returns (bool) {
        return true;
    }
}
