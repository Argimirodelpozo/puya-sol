// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// xchain account model (--xchain-template): payments to 160-bit identities
// route to the owner's derived LogicSig account; a caller that IS the
// derived account claims its owner as msg.sender via ApplicationArgs[2].
contract XchainKit {
    mapping(address => uint256) public bal;

    receive() external payable {}

    function payout(address to, uint256 amount) external {
        payable(to).transfer(amount);
    }

    function whoami() external view returns (address) {
        return msg.sender;
    }

    function deposit(uint256 amount) external {
        // keyed on msg.sender: an xchain caller's claim keys on the TRUE
        // EVM identity, so an off-chain signer and an on-chain caller with
        // the same key hit the same slot.
        bal[msg.sender] += amount;
    }
}
