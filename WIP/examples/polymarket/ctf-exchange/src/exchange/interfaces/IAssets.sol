// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

abstract contract IAssets {
    function getCollateral() public virtual returns (address);

    function getCtf() public virtual returns (address);
}
