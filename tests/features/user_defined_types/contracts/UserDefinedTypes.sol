// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// User-defined value types
type TokenAmount is uint256;
type Percentage is uint256;
type UserId is uint64;

// Using-for with free functions for operator overloading
function addTokens(TokenAmount a, TokenAmount b) pure returns (TokenAmount) {
    return TokenAmount.wrap(TokenAmount.unwrap(a) + TokenAmount.unwrap(b));
}

function subTokens(TokenAmount a, TokenAmount b) pure returns (TokenAmount) {
    return TokenAmount.wrap(TokenAmount.unwrap(a) - TokenAmount.unwrap(b));
}

function eqTokens(TokenAmount a, TokenAmount b) pure returns (bool) {
    return TokenAmount.unwrap(a) == TokenAmount.unwrap(b);
}

using {addTokens as +, subTokens as -, eqTokens as ==} for TokenAmount global;

contract UserDefinedTypes {
    TokenAmount public totalSupply;
    mapping(address => TokenAmount) public balances;

    function mint(address to, TokenAmount amount) external {
        balances[to] = balances[to] + amount;
        totalSupply = totalSupply + amount;
    }

    function transfer(address from, address to, TokenAmount amount) external {
        balances[from] = balances[from] - amount;
        balances[to] = balances[to] + amount;
    }

    function getBalance(address who) external view returns (uint256) {
        return TokenAmount.unwrap(balances[who]);
    }

    function getTotalSupply() external view returns (uint256) {
        return TokenAmount.unwrap(totalSupply);
    }

    // Wrap/unwrap roundtrip
    function wrapUnwrap(uint256 val) external pure returns (uint256) {
        TokenAmount t = TokenAmount.wrap(val);
        return TokenAmount.unwrap(t);
    }

    // UDVT with uint64 underlying
    function createUserId(uint64 id) external pure returns (uint64) {
        UserId uid = UserId.wrap(id);
        return UserId.unwrap(uid);
    }

    // Percentage computation
    function applyPercentage(uint256 amount, uint256 pct) external pure returns (uint256) {
        Percentage p = Percentage.wrap(pct);
        return amount * Percentage.unwrap(p) / 100;
    }
}
