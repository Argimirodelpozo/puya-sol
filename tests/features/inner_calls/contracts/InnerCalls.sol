// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

interface ICounter {
    function getValue() external view returns (uint256);
    function increment() external;
    function add(uint256 amount) external;
}

interface IOracle {
    function getPrice() external view returns (uint256);
    function isOracle() external view returns (bool);
}

/// @title Counter contract — called by Caller via inner transactions.
contract Counter {
    uint256 public value;

    function getValue() external view returns (uint256) {
        return value;
    }

    function increment() external {
        value += 1;
    }

    function add(uint256 amount) external {
        value += amount;
    }

    function isOracle() external pure returns (bool) {
        return false;
    }
}

/// @title Oracle mock — returns a configurable price.
contract Oracle {
    uint256 public price;

    function setPrice(uint256 _price) external {
        price = _price;
    }

    function getPrice() external view returns (uint256) {
        return price;
    }

    function isOracle() external pure returns (bool) {
        return true;
    }
}

/// @title Caller contract — makes inner app calls to Counter and Oracle.
contract Caller {
    address public counterAddr;
    address public oracleAddr;

    function setCounter(address _counter) external {
        counterAddr = _counter;
    }

    function setOracle(address _oracle) external {
        oracleAddr = _oracle;
    }

    // Read from Counter via interface call
    function readCounter() external view returns (uint256) {
        return ICounter(counterAddr).getValue();
    }

    // Write to Counter via interface call
    function callIncrement() external {
        ICounter(counterAddr).increment();
    }

    // Write with argument
    function callAdd(uint256 amount) external {
        ICounter(counterAddr).add(amount);
    }

    // Read from Oracle
    function readPrice() external view returns (uint256) {
        return IOracle(oracleAddr).getPrice();
    }

    // Check bool return from concrete contract
    function checkIsOracle() external view returns (bool) {
        return IOracle(oracleAddr).isOracle();
    }

    // Multiple inner calls in one transaction
    function incrementTwice() external {
        ICounter(counterAddr).increment();
        ICounter(counterAddr).increment();
    }

    // Read + compute
    function doublePrice() external view returns (uint256) {
        uint256 p = IOracle(oracleAddr).getPrice();
        return p * 2;
    }
}
