// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the monetary-amount overflow check (fable-review-3 M17): a uint256
// amount >= 2^64 can't be represented in the AVM's 64-bit amount field.
// Pre-fix, `.transfer`/`.send`/`{value:}` and ASA amounts silently took the
// low 8 bytes — `transfer(100 ether)` (1e20 > 2^64) sent 1e20 mod 2^64
// microAlgos. Now such amounts REVERT instead of moving a wrong quantity.
contract AmountOverflowGuard {
    // amount is uint256; a fitting value transfers, a >2^64 value reverts.
    function doTransfer(address payable to, uint256 amount) external {
        to.transfer(amount);
    }

    function doSend(address payable to, uint256 amount) external returns (bool) {
        return to.send(amount);
    }

    function doValueCall(address to, uint256 amount) external returns (bool) {
        (bool ok, ) = to.call{value: amount}("");
        return ok;
    }
}
