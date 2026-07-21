// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards M8 remainders: asm call's `value` attaches a grouped payment (was
// silently dropped — the callee's msg.value saw nothing), and the output copy
// + returndatasize see the ARC4 return prefix stripped (EVM-shaped returndata).
contract AsmCallValueCallee {
    uint256 public hits;
    uint256 public got;

    function ping(uint256 x) external returns (uint256) {
        hits += 1;
        return x + 1000;
    }

    fallback() external payable {
        hits += 100;
        got = msg.value;
    }

    receive() external payable {}
}

contract AsmCallValueCaller {
    // M8-value: plain value transfer (Solady safeTransferETH shape) — the
    // garbage 4-byte selector routes to the callee's fallback, whose
    // msg.value must see the grouped payment.
    // M8-output: selector-addressed call; r must be the RAW return value
    // (prefix stripped) and rds its EVM-shaped size.
    function run(uint256 amt, uint256 x)
        external
        payable
        returns (uint256 ok, uint256 hits1, uint256 got1, uint256 r, uint256 rds)
    {
        AsmCallValueCallee c = new AsmCallValueCallee();
        address t = address(c);
        uint32 selU = uint32(AsmCallValueCallee.ping.selector);
        assembly {
            ok := call(gas(), t, amt, 0, 0, 0, 0)
        }
        hits1 = c.hits();
        got1 = c.got();
        assembly {
            mstore(0x00, selU) // right-aligned: selector at 0x1c..0x20
            mstore(0x20, x)
            let ok2 := call(gas(), t, 0, 0x1c, 0x24, 0x40, 0x20)
            r := mload(0x40)
            rds := returndatasize()
        }
    }
}
