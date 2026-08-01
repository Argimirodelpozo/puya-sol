// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
import "./profiles/TextResolver.sol";
contract TestTextResolver is TextResolver {
    function isAuthorised(bytes32) internal view override returns (bool) { return true; }
}
