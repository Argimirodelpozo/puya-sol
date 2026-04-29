import { expect } from "chai";
import { ethers } from "hardhat";
import { HardhatEthersSigner } from "@nomicfoundation/hardhat-ethers/signers";
import { Airdrop } from "../typechain-types";

describe("Airdrop", () => {
  let owner: HardhatEthersSigner;
  let nonOwner: HardhatEthersSigner;
  let newOwner: HardhatEthersSigner;
  let airdrop: Airdrop;
  let tokenAddress: string;
  let hubAddress: string;

  beforeEach(async () => {
    [owner, nonOwner, newOwner] = await ethers.getSigners();

    // Use a dummy address for the token. The token is immutable and only used
    // in claim(), which requires ZK proofs + merkle proofs beyond this test scope.
    tokenAddress = ethers.Wallet.createRandom().address;

    // Use a dummy address for the identity verification hub as well.
    // The hub is only called during verifySelfProof, which requires ZK proofs.
    hubAddress = ethers.Wallet.createRandom().address;

    const AirdropFactory = await ethers.getContractFactory("Airdrop");
    airdrop = await AirdropFactory.deploy(
      hubAddress,       // identityVerificationHubAddress
      "test-scope",     // scopeSeed
      tokenAddress      // tokenAddress
    );
    await airdrop.waitForDeployment();
  });

  // ====================================================
  // Owner management
  // ====================================================

  describe("Owner management", () => {
    it("should set the deployer as owner", async () => {
      expect(await airdrop.owner()).to.equal(owner.address);
    });

    it("should transfer ownership to a new address", async () => {
      await airdrop.connect(owner).transferOwnership(newOwner.address);
      expect(await airdrop.owner()).to.equal(newOwner.address);
    });

    it("should emit OwnershipTransferred event on transfer", async () => {
      await expect(airdrop.connect(owner).transferOwnership(newOwner.address))
        .to.emit(airdrop, "OwnershipTransferred")
        .withArgs(owner.address, newOwner.address);
    });

    it("should not allow non-owner to transfer ownership", async () => {
      await expect(
        airdrop.connect(nonOwner).transferOwnership(newOwner.address)
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should renounce ownership", async () => {
      await airdrop.connect(owner).renounceOwnership();
      expect(await airdrop.owner()).to.equal(ethers.ZeroAddress);
    });

    it("should emit OwnershipTransferred event on renounce", async () => {
      await expect(airdrop.connect(owner).renounceOwnership())
        .to.emit(airdrop, "OwnershipTransferred")
        .withArgs(owner.address, ethers.ZeroAddress);
    });

    it("should not allow non-owner to renounce ownership", async () => {
      await expect(
        airdrop.connect(nonOwner).renounceOwnership()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should not allow owner actions after renouncing ownership", async () => {
      await airdrop.connect(owner).renounceOwnership();
      await expect(
        airdrop.connect(owner).openRegistration()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should allow new owner to perform owner actions after transfer", async () => {
      await airdrop.connect(owner).transferOwnership(newOwner.address);
      // New owner should be able to open registration
      await expect(airdrop.connect(newOwner).openRegistration()).to.not.be.reverted;
    });

    it("should not allow previous owner to perform actions after transfer", async () => {
      await airdrop.connect(owner).transferOwnership(newOwner.address);
      await expect(
        airdrop.connect(owner).openRegistration()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });
  });

  // ====================================================
  // Registration phase management
  // ====================================================

  describe("Registration phase management", () => {
    it("should start with registration closed", async () => {
      expect(await airdrop.isRegistrationOpen()).to.be.false;
    });

    it("should open registration", async () => {
      await airdrop.connect(owner).openRegistration();
      expect(await airdrop.isRegistrationOpen()).to.be.true;
    });

    it("should emit RegistrationOpen event", async () => {
      await expect(airdrop.connect(owner).openRegistration())
        .to.emit(airdrop, "RegistrationOpen");
    });

    it("should close registration", async () => {
      await airdrop.connect(owner).openRegistration();
      await airdrop.connect(owner).closeRegistration();
      expect(await airdrop.isRegistrationOpen()).to.be.false;
    });

    it("should emit RegistrationClose event", async () => {
      await airdrop.connect(owner).openRegistration();
      await expect(airdrop.connect(owner).closeRegistration())
        .to.emit(airdrop, "RegistrationClose");
    });

    it("should not allow non-owner to open registration", async () => {
      await expect(
        airdrop.connect(nonOwner).openRegistration()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should not allow non-owner to close registration", async () => {
      await expect(
        airdrop.connect(nonOwner).closeRegistration()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should allow opening registration multiple times (idempotent)", async () => {
      await airdrop.connect(owner).openRegistration();
      await airdrop.connect(owner).openRegistration();
      expect(await airdrop.isRegistrationOpen()).to.be.true;
    });

    it("should allow closing registration when already closed", async () => {
      await airdrop.connect(owner).closeRegistration();
      expect(await airdrop.isRegistrationOpen()).to.be.false;
    });
  });

  // ====================================================
  // Claim phase management
  // ====================================================

  describe("Claim phase management", () => {
    it("should start with claim closed", async () => {
      expect(await airdrop.isClaimOpen()).to.be.false;
    });

    it("should open claim", async () => {
      await airdrop.connect(owner).openClaim();
      expect(await airdrop.isClaimOpen()).to.be.true;
    });

    it("should emit ClaimOpen event", async () => {
      await expect(airdrop.connect(owner).openClaim())
        .to.emit(airdrop, "ClaimOpen");
    });

    it("should close claim", async () => {
      await airdrop.connect(owner).openClaim();
      await airdrop.connect(owner).closeClaim();
      expect(await airdrop.isClaimOpen()).to.be.false;
    });

    it("should emit ClaimClose event", async () => {
      await airdrop.connect(owner).openClaim();
      await expect(airdrop.connect(owner).closeClaim())
        .to.emit(airdrop, "ClaimClose");
    });

    it("should not allow non-owner to open claim", async () => {
      await expect(
        airdrop.connect(nonOwner).openClaim()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should not allow non-owner to close claim", async () => {
      await expect(
        airdrop.connect(nonOwner).closeClaim()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should allow opening claim multiple times (idempotent)", async () => {
      await airdrop.connect(owner).openClaim();
      await airdrop.connect(owner).openClaim();
      expect(await airdrop.isClaimOpen()).to.be.true;
    });

    it("should allow closing claim when already closed", async () => {
      await airdrop.connect(owner).closeClaim();
      expect(await airdrop.isClaimOpen()).to.be.false;
    });
  });

  // ====================================================
  // Merkle root configuration
  // ====================================================

  describe("Merkle root configuration", () => {
    const merkleRoot = ethers.keccak256(ethers.toUtf8Bytes("test-merkle-root"));

    it("should start with zero merkle root", async () => {
      expect(await airdrop.merkleRoot()).to.equal(ethers.ZeroHash);
    });

    it("should set the merkle root", async () => {
      await airdrop.connect(owner).setMerkleRoot(merkleRoot);
      expect(await airdrop.merkleRoot()).to.equal(merkleRoot);
    });

    it("should emit MerkleRootUpdated event", async () => {
      await expect(airdrop.connect(owner).setMerkleRoot(merkleRoot))
        .to.emit(airdrop, "MerkleRootUpdated")
        .withArgs(merkleRoot);
    });

    it("should not allow non-owner to set merkle root", async () => {
      await expect(
        airdrop.connect(nonOwner).setMerkleRoot(merkleRoot)
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should allow updating the merkle root", async () => {
      const newRoot = ethers.keccak256(ethers.toUtf8Bytes("new-root"));
      await airdrop.connect(owner).setMerkleRoot(merkleRoot);
      await airdrop.connect(owner).setMerkleRoot(newRoot);
      expect(await airdrop.merkleRoot()).to.equal(newRoot);
    });

    it("should allow setting merkle root to zero", async () => {
      await airdrop.connect(owner).setMerkleRoot(merkleRoot);
      await airdrop.connect(owner).setMerkleRoot(ethers.ZeroHash);
      expect(await airdrop.merkleRoot()).to.equal(ethers.ZeroHash);
    });
  });

  // ====================================================
  // Verification config ID
  // ====================================================

  describe("Verification config ID", () => {
    const configId = ethers.keccak256(ethers.toUtf8Bytes("test-config-id"));

    it("should start with zero verification config ID", async () => {
      expect(await airdrop.verificationConfigId()).to.equal(ethers.ZeroHash);
    });

    it("should set the verification config ID", async () => {
      await airdrop.connect(owner).setConfigId(configId);
      expect(await airdrop.verificationConfigId()).to.equal(configId);
    });

    it("should not allow non-owner to set config ID", async () => {
      await expect(
        airdrop.connect(nonOwner).setConfigId(configId)
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should allow updating the config ID", async () => {
      const newConfigId = ethers.keccak256(ethers.toUtf8Bytes("new-config"));
      await airdrop.connect(owner).setConfigId(configId);
      await airdrop.connect(owner).setConfigId(newConfigId);
      expect(await airdrop.verificationConfigId()).to.equal(newConfigId);
    });

    it("getConfigId should return the stored verification config ID", async () => {
      await airdrop.connect(owner).setConfigId(configId);

      const dummyChainId = ethers.ZeroHash;
      const dummyUserIdentifier = ethers.ZeroHash;
      const dummyData = "0x";

      const result = await airdrop.getConfigId(
        dummyChainId,
        dummyUserIdentifier,
        dummyData
      );
      expect(result).to.equal(configId);
    });

    it("getConfigId should return the same value regardless of parameters", async () => {
      await airdrop.connect(owner).setConfigId(configId);

      const randomChainId = ethers.keccak256(ethers.toUtf8Bytes("chain-1"));
      const randomUser = ethers.keccak256(ethers.toUtf8Bytes("user-1"));
      const randomData = ethers.toUtf8Bytes("some-data");

      const result = await airdrop.getConfigId(
        randomChainId,
        randomUser,
        randomData
      );
      expect(result).to.equal(configId);
    });
  });

  // ====================================================
  // Scope
  // ====================================================

  describe("Scope", () => {
    it("getScope should return a value", async () => {
      const scope = await airdrop.getScope();
      // On a local dev network, PoseidonT3 address is zero, so scope should be 0
      expect(scope).to.equal(0n);
    });

    it("scope() should return the same value as getScope()", async () => {
      const getScoreResult = await airdrop.getScope();
      const scopeResult = await airdrop.scope();
      expect(getScoreResult).to.equal(scopeResult);
    });
  });

  // ====================================================
  // isRegistered
  // ====================================================

  describe("isRegistered", () => {
    it("should return false for unregistered addresses", async () => {
      expect(await airdrop.isRegistered(owner.address)).to.be.false;
      expect(await airdrop.isRegistered(nonOwner.address)).to.be.false;
    });

    it("should return false for the zero address", async () => {
      expect(await airdrop.isRegistered(ethers.ZeroAddress)).to.be.false;
    });
  });

  // ====================================================
  // Claim guards (without full ZK/merkle flow)
  // ====================================================

  describe("Claim guards", () => {
    it("should revert claim when registration is still open", async () => {
      await airdrop.connect(owner).openRegistration();
      await airdrop.connect(owner).openClaim();

      await expect(
        airdrop.connect(nonOwner).claim(0, 100, [])
      ).to.be.revertedWithCustomError(airdrop, "RegistrationNotClosed");
    });

    it("should revert claim when claim is not open", async () => {
      // Registration closed (default), claim closed (default)
      await expect(
        airdrop.connect(nonOwner).claim(0, 100, [])
      ).to.be.revertedWithCustomError(airdrop, "ClaimNotOpen");
    });

    it("should revert claim when user is not registered", async () => {
      // Registration closed, claim open
      await airdrop.connect(owner).openClaim();

      await expect(
        airdrop.connect(nonOwner).claim(0, 100, [])
      ).to.be.revertedWithCustomError(airdrop, "NotRegistered");
    });
  });

  // ====================================================
  // Token immutable
  // ====================================================

  describe("Token address", () => {
    it("should return the token address set in constructor", async () => {
      expect(await airdrop.token()).to.equal(tokenAddress);
    });
  });

  // ====================================================
  // Full lifecycle integration
  // ====================================================

  describe("Full lifecycle (phase transitions)", () => {
    it("should support a complete owner-driven lifecycle", async () => {
      // 1. Initially everything is closed
      expect(await airdrop.isRegistrationOpen()).to.be.false;
      expect(await airdrop.isClaimOpen()).to.be.false;

      // 2. Set merkle root
      const merkleRoot = ethers.keccak256(ethers.toUtf8Bytes("airdrop-root"));
      await airdrop.connect(owner).setMerkleRoot(merkleRoot);
      expect(await airdrop.merkleRoot()).to.equal(merkleRoot);

      // 3. Set config ID
      const configId = ethers.keccak256(ethers.toUtf8Bytes("config-1"));
      await airdrop.connect(owner).setConfigId(configId);
      expect(await airdrop.verificationConfigId()).to.equal(configId);

      // 4. Open registration
      await airdrop.connect(owner).openRegistration();
      expect(await airdrop.isRegistrationOpen()).to.be.true;

      // 5. Close registration
      await airdrop.connect(owner).closeRegistration();
      expect(await airdrop.isRegistrationOpen()).to.be.false;

      // 6. Open claim
      await airdrop.connect(owner).openClaim();
      expect(await airdrop.isClaimOpen()).to.be.true;

      // 7. Close claim
      await airdrop.connect(owner).closeClaim();
      expect(await airdrop.isClaimOpen()).to.be.false;
    });

    it("should allow re-opening phases after closing", async () => {
      await airdrop.connect(owner).openRegistration();
      await airdrop.connect(owner).closeRegistration();
      await airdrop.connect(owner).openRegistration();
      expect(await airdrop.isRegistrationOpen()).to.be.true;

      await airdrop.connect(owner).openClaim();
      await airdrop.connect(owner).closeClaim();
      await airdrop.connect(owner).openClaim();
      expect(await airdrop.isClaimOpen()).to.be.true;
    });
  });

  // ====================================================
  // Access control comprehensive check
  // ====================================================

  describe("Access control (non-owner rejection for all restricted functions)", () => {
    it("should reject non-owner for setMerkleRoot", async () => {
      await expect(
        airdrop.connect(nonOwner).setMerkleRoot(ethers.ZeroHash)
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should reject non-owner for openRegistration", async () => {
      await expect(
        airdrop.connect(nonOwner).openRegistration()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should reject non-owner for closeRegistration", async () => {
      await expect(
        airdrop.connect(nonOwner).closeRegistration()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should reject non-owner for openClaim", async () => {
      await expect(
        airdrop.connect(nonOwner).openClaim()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should reject non-owner for closeClaim", async () => {
      await expect(
        airdrop.connect(nonOwner).closeClaim()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should reject non-owner for setConfigId", async () => {
      await expect(
        airdrop.connect(nonOwner).setConfigId(ethers.ZeroHash)
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should reject non-owner for transferOwnership", async () => {
      await expect(
        airdrop.connect(nonOwner).transferOwnership(nonOwner.address)
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });

    it("should reject non-owner for renounceOwnership", async () => {
      await expect(
        airdrop.connect(nonOwner).renounceOwnership()
      ).to.be.revertedWithCustomError(airdrop, "OwnableUnauthorizedAccount");
    });
  });
});
