// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract OrderBook {
    struct Order { address maker; uint256 price; uint256 amount; }
    Order[] public orders;
    mapping(uint256 => uint256) public idToIndex; // orderId -> array index
    uint256 public nextId;

    function place(uint256 price, uint256 amount) external returns (uint256 id) {
        id = nextId++;
        idToIndex[id] = orders.length;
        orders.push(Order(msg.sender, price, amount));
    }
    function cancel(uint256 id) external {
        uint256 idx = idToIndex[id];
        uint256 last = orders.length - 1;
        if (idx != last) {
            orders[idx] = orders[last];        // swap-and-pop (struct copy)
        }
        orders.pop();
    }
    function fillPartial(uint256 idx, uint256 amt) external {
        orders[idx].amount -= amt;             // struct-field compound assign in array
    }
    function reprice(uint256 i, uint256 j) external {
        (orders[i].price, orders[j].price) = (orders[j].price, orders[i].price); // struct-field-in-array swap
    }
    function total() external view returns (uint256 sum) {
        for (uint256 i = 0; i < orders.length; i++) sum += orders[i].amount;
    }
    function len() external view returns (uint256) { return orders.length; }
}
