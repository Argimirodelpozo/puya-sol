// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title Abstract base with virtual methods and constructor args.
abstract contract Animal {
    string public species;
    uint256 public legs;

    constructor(string memory _species, uint256 _legs) {
        species = _species;
        legs = _legs;
    }

    function sound() external virtual returns (string memory);

    function describe() external view returns (string memory) {
        return species;
    }

    function legCount() external view returns (uint256) {
        return legs;
    }
}

/// @title Multiple inheritance with abstract + concrete base.
abstract contract Named {
    string public name;

    function setName(string memory _name) external {
        name = _name;
    }

    function greet() external virtual returns (string memory);
}

/// @title Concrete implementation of abstract Animal.
contract Dog is Animal {
    constructor() Animal("Canine", 4) {}

    function sound() external pure override returns (string memory) {
        return "Woof";
    }
}

/// @title Concrete with two abstract bases.
contract NamedDog is Animal, Named {
    constructor() Animal("Dog", 4) {}

    function sound() external pure override returns (string memory) {
        return "Bark";
    }

    function greet() external pure override returns (string memory) {
        return "Hello, I am a dog";
    }
}

/// @title Abstract with state and internal helper.
abstract contract Ownable {
    address public owner;

    constructor() {
        owner = msg.sender;
    }

    modifier onlyOwner() {
        require(msg.sender == owner, "not owner");
        _;
    }

    function transferOwnership(address newOwner) external onlyOwner {
        require(newOwner != address(0), "zero address");
        owner = newOwner;
    }
}

/// @title Concrete combining Ownable + custom logic.
contract OwnedVault is Ownable {
    uint256 public balance;

    function deposit(uint256 amount) external {
        balance += amount;
    }

    function withdraw(uint256 amount) external onlyOwner {
        require(amount <= balance, "insufficient");
        balance -= amount;
    }
}
