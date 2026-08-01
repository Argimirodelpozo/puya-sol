// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
import "./profiles/InterfaceResolver.sol";
contract TestInterfaceResolver is InterfaceResolver {
    function isAuthorised(bytes32) internal view override returns (bool) { return true; }
    function supportsInterface(bytes4 id) public view override(InterfaceResolver) returns (bool) {
        return super.supportsInterface(id);
    }
}
