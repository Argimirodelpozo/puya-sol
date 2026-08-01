// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
import "./profiles/ABIResolver.sol";
contract TestABIResolver is ABIResolver {
    function isAuthorised(bytes32) internal view override returns (bool) { return true; }
}
