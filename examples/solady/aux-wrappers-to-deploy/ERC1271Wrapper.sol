// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/accounts/ERC1271.sol";

contract ERC1271Wrapper is ERC1271 {
    address private _signer;

    constructor() {
        _signer = msg.sender;
    }

    function _domainNameAndVersion() internal pure override returns (string memory name, string memory version) {
        name = "Test";
        version = "1";
    }

    function _erc1271Signer() internal view override returns (address) {
        return _signer;
    }

    function checkSignature(bytes32 hash, bytes calldata signature) external view returns (bytes4) {
        return isValidSignature(hash, signature);
    }
}
