// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

import {PercentageMath} from "./PercentageMath.sol";

contract PercentageMathWrapper {
  function PERCENTAGE_FACTOR() public pure returns (uint256) {
    return PercentageMath.PERCENTAGE_FACTOR;
  }

  function percentMulDown(uint256 value, uint256 percentage) public pure returns (uint256) {
    return PercentageMath.percentMulDown(value, percentage);
  }

  function percentMulUp(uint256 value, uint256 percentage) public pure returns (uint256) {
    return PercentageMath.percentMulUp(value, percentage);
  }

  function percentDivDown(uint256 value, uint256 percentage) public pure returns (uint256) {
    return PercentageMath.percentDivDown(value, percentage);
  }

  function percentDivUp(uint256 value, uint256 percentage) public pure returns (uint256) {
    return PercentageMath.percentDivUp(value, percentage);
  }

  function fromBpsDown(uint256 bps) public pure returns (uint256) {
    return PercentageMath.fromBpsDown(bps);
  }
}
