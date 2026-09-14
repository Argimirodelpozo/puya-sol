// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract EntryGuardBase {
    uint256 public total;

    function price(uint256 n) public pure virtual returns (uint256) {
        return n * 2;
    }
}

contract EntryGuardCompaction is EntryGuardBase {
    function price(uint256 n) public pure override returns (uint256) {
        return super.price(n) + 1;
    }

    function plain(uint256 n) public returns (uint256) {
        total += n;
        return total;
    }

    function paid(uint256 n) public payable returns (uint256) {
        function(uint256) internal pure returns (uint256) quote = price;
        return plain(price(n) + quote(n) + msg.value);
    }

    modifier skip() { return; _; }
    function skipped() public skip {}

    receive() external payable { total += msg.value; }
    fallback() external { total += 100; }
}

contract PayableFallbackCompaction {
    uint256 public total;
    fallback() external payable { total += msg.value; }
}
