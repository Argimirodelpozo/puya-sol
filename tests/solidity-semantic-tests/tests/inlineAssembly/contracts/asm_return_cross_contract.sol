// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

interface ISettle { function vaultRelayer() external view returns (address); }
interface IWrap { function approve(address s, uint256 v) external returns (bool); }

/// the stand-in, verbatim shape: known selectors + catch-all fallback
contract Stub {
    mapping(address => mapping(address => uint256)) public allowance;
    function approve(address s, uint256 v) external returns (bool) {
        allowance[msg.sender][s] = v;
        return true;
    }
    fallback() external payable {
        assembly { mstore(0x00, address()) return(0x00, 0x20) }
    }
    receive() external payable {}
}

/// the CoWSwapEthFlow ctor chain, distilled
contract Seam {
    address public got;
    constructor(address settle, address wrap) {
        address r = ISettle(settle).vaultRelayer();   // fallback answers
        got = r;
        IWrap(wrap).approve(r, type(uint256).max);    // typed call with it
    }
}
