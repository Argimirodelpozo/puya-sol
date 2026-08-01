// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
import "./profiles/AddrResolver.sol";
import "./profiles/TextResolver.sol";
import "./profiles/ContentHashResolver.sol";
import "./profiles/NameResolver.sol";
import "./profiles/PubkeyResolver.sol";

// The five simple ENS resolver profiles combined (like PublicResolver, minus
// DNS/Interface/ABI/Multicallable/ReverseClaimer). Tests multiple-inheritance,
// supportsInterface chaining, and combined storage layout. Open auth for testing.
contract CoreResolver is
    AddrResolver,
    TextResolver,
    ContentHashResolver,
    NameResolver,
    PubkeyResolver
{
    function isAuthorised(bytes32) internal view override returns (bool) { return true; }

    function supportsInterface(bytes4 interfaceID)
        public view override(
            AddrResolver, TextResolver, ContentHashResolver, NameResolver, PubkeyResolver
        ) returns (bool)
    {
        return super.supportsInterface(interfaceID);
    }
}
