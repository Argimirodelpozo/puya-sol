# HONK Behavioral Test Fixtures

## Status: Infrastructure Only

The HONK verifier behavioral tests require:

1. **Budget pooling**: `ec_pairing_check` needs ~28,000 opcodes (2-pair check).
   AVM budget: 700/app call × 16 in group = 11,200. Need inner transaction budget pooling.

2. **Test vectors**: Available from [miquelcabot/ultrahonk_verifier](https://github.com/miquelcabot/ultrahonk_verifier)
   - Circuit: N=32, LOG_N=5, 2 public inputs (values 2 and 3)
   - VK: 55 field elements (1,760 bytes) — see `should.rs` valid_vk fixture
   - Plain proof: 440 field elements (14,080 bytes) — see `should.rs` valid_plain_proof fixture
   - These are for a SIMPLE circuit, NOT the ECDSA circuit (N=65536, 6 inputs)

3. **ECDSA proof generation**: Requires building barretenberg C++ binary:
   ```bash
   # From barretenberg repo:
   bb write_vk -b ./target/ecdsa.json -o ./target --oracle_hash keccak
   bb prove -b ./target/ecdsa.json -w ./target/witness -o ./target --oracle_hash keccak
   ```

4. **Orchestration**: The split verifier (61 contracts) needs the orchestrator to
   coordinate calls to all 60 helpers via inner transactions.

## verify() Signature

```solidity
function verify(bytes calldata proof, bytes32[] calldata publicInputs) public view returns (bool)
```

- VK is hardcoded in the contract (not passed as parameter)
- Proof: ~14KB for plain, ~15.7KB for ZK
- Public inputs: array of bytes32 field elements

## Next Steps

1. Implement budget pooling via inner transactions in the orchestrator
2. Create SimpleHonkVerifier with N=32 VK for initial testing
3. Deploy and test with the simple circuit vectors
4. Build barretenberg for ECDSA proof generation
