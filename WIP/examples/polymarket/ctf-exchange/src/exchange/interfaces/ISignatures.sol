// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

import { Order } from "../libraries/OrderStructs.sol";

interface ISignaturesEE {
    error InvalidSignature();
}

abstract contract ISignatures is ISignaturesEE {
    function validateOrderSignature(bytes32 orderHash, Order memory order) public view virtual;
}
