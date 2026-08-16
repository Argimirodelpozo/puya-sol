/// @file SolIntrinsicAccess.cpp
/// msg.sender, block.timestamp, block.prevrandao, block.difficulty, etc.

#include "builder/sol-ast/members/SolIntrinsicAccess.h"
#include "builder/SelectorSemantics.h"
#include "builder/sol-intrinsics/IntrinsicMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <vector>

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolIntrinsicAccess::toAwst()
{
	auto const* baseId = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpression());
	if (!baseId) return nullptr;

	std::string baseName = baseId->name();
	std::string member = memberName();

	// block.chainid → 1 (no per-chain id on AVM). Warn: the constant 1 is
	// folded into EIP-712/permit domain separators on every Algorand network,
	// enabling cross-network replay. Use `global GenesisHash` in assembly instead.
	if (baseName == "block" && member == "chainid")
	{
		Logger::instance().warning(
			"block.chainid returns the constant 1 on AVM (no per-chain id). "
			"EIP-712/permit domain separators will be identical across Algorand "
			"networks — signatures may be replayable cross-network. Read global "
			"GenesisHash in assembly to distinguish networks.", m_loc);
		auto c = awst::makeOne(m_loc, awst::WType::biguintType());
		return c;
	}

	// block.difficulty → 0 (no PoW on Algorand)
	if (baseName == "block" && member == "difficulty")
	{
		Logger::instance().warning(
			"block.difficulty returns 0 on AVM — Algorand has no proof-of-work.", m_loc);
		auto zero = awst::makeZero(m_loc, awst::WType::biguintType());
		return zero;
	}

	// block.basefee / block.blobbasefee → 0.
	// AVM has a flat per-txn fee (~1000 microAlgos); no EIP-1559 or blob pricing.
	if (baseName == "block" && (member == "basefee" || member == "blobbasefee"))
	{
		Logger::instance().warning(
			"block." + member + " returns 0 on AVM — no EIP-1559 base fee concept.", m_loc);
		auto zero = awst::makeZero(m_loc, awst::WType::biguintType());
		return zero;
	}

	// block.gaslimit → 70000 sentinel. AVM has no gas (fixed opcode budget:
	// 700/call, poolable across a 16-txn group). 70000 prevents premature
	// abort in gaslimit-based Solidity loops.
	if (baseName == "block" && member == "gaslimit")
	{
		Logger::instance().warning(
			"block.gaslimit returns 70000 on AVM — no direct analog for EVM block gas limit.", m_loc);
		auto val = awst::makeIntegerConstant("70000", m_loc, awst::WType::biguintType());
		return val;
	}

	// block.prevrandao → block BlkSeed (Round - 2)
	if (baseName == "block" && member == "prevrandao")
	{
		Logger::instance().warning(
			"block.prevrandao mapped to AVM block seed (BlkSeed) of previous round.", m_loc);

		auto round = awst::makeGlobal(std::string("Round"), awst::WType::uint64Type(), m_loc);

		auto two = awst::makeIntegerConstant("2", m_loc);

		auto prevRound = awst::makeUInt64BinOp(std::move(round), awst::UInt64BinaryOperator::Sub, std::move(two), m_loc);

		auto blockSeed = awst::makeBlock(
			"BlkSeed", std::move(prevRound), awst::WType::bytesType(), m_loc);

		auto cast = awst::makeAsBiguint(std::move(blockSeed), m_loc);
		return cast;
	}

	// msg.value → GroupIndex > 0 ? gtxns Amount[GroupIndex-1] : 0
	if (baseName == "msg" && member == "value")
	{
		auto groupIdx = awst::makeTxn(std::string("GroupIndex"), awst::WType::uint64Type(), m_loc);

		auto zero = awst::makeZero(m_loc);
		auto hasPayment = awst::makeNumericCompare(groupIdx, awst::NumericComparison::Gt, std::move(zero), m_loc);

		auto groupIdx2 = awst::makeTxn(std::string("GroupIndex"), awst::WType::uint64Type(), m_loc);
		auto one = awst::makeOne(m_loc);
		auto payIdx = awst::makeUInt64BinOp(std::move(groupIdx2), awst::UInt64BinaryOperator::Sub, std::move(one), m_loc);

		auto amount = awst::makeGtxns(
			"Amount", std::move(payIdx), awst::WType::uint64Type(), m_loc);

		auto zeroVal = awst::makeZero(m_loc);

		auto cond = awst::makeConditional(
			std::move(hasPayment), std::move(amount), std::move(zeroVal),
			awst::WType::uint64Type(), m_loc);

			// Promote uint64 → biguint (Solidity msg.value is uint256)
		auto itob = awst::makeItob(std::move(cond), m_loc);
		return awst::makeAsBiguint(std::move(itob), m_loc);
	}

	// msg.sig is the routed ARC-4 selector in compatibility mode. Under
	// --evm-selectors the explicit transport map recovers the corresponding
	// Solidity selector while preserving the outer selector across internal calls.
	if (baseName == "msg" && member == "sig")
	{
		if (!builder::SelectorSemantics::enabled(m_ctx.typeMapper))
			return awst::makeAppArg(
				0, m_loc, m_ctx.typeMapper.createType<awst::BytesWType>(4));

		auto hasSelector = awst::makeNumericCompare(
			awst::makeTxn(
				std::string("NumAppArgs"), awst::WType::uint64Type(), m_loc),
			awst::NumericComparison::Gt, awst::makeZero(m_loc), m_loc);
		auto raw = awst::makeConditional(
			std::move(hasSelector), awst::makeAppArg(0, m_loc),
			awst::makeBytesConstant(std::vector<uint8_t>(4, 0), m_loc),
			awst::WType::bytesType(), m_loc);
		auto selector = builder::SelectorSemantics::runtimeSelector(
			m_ctx, std::move(raw), m_loc);
		return awst::makeReinterpretCast(
			std::move(selector),
			m_ctx.typeMapper.createType<awst::BytesWType>(4), m_loc);
	}

	// msg.data → concatenate ApplicationArgs[0..15] (selector + ARC4 args).
	// ARC4 args are already left-padded; result approximates EVM head encoding
	// for scalar args. Bare calls return bzero(0). Cap at 16 slots (AVM hard limit).
	if (baseName == "msg" && member == "data")
	{
		// EVM calldata is empty during construction (ctor args in initcode, not calldata).
		// AVM runs ctor as __postInit with ApplicationArgs; return empty to match
		// (various/create_calldata asserts msg.data.length == 0 in the ctor).
		if (m_scope.isInConstructor())
			return awst::makeBytesConstant({}, m_loc);

		auto numAppArgs = awst::makeTxn(std::string("NumAppArgs"), awst::WType::uint64Type(), m_loc);

		auto zero = awst::makeZero(m_loc);

		auto hasData = awst::makeNumericCompare(std::move(numAppArgs), awst::NumericComparison::Gt, std::move(zero), m_loc);

		// Concatenate slots 0..15; absent slots contribute bzero(0).
		std::shared_ptr<awst::Expression> calldataConcat;
		for (int slot = 0; slot < 16; ++slot)
		{
			auto slotIdx = awst::makeIntegerConstant(slot, m_loc);

			auto numArgsCheck = awst::makeTxn(std::string("NumAppArgs"), awst::WType::uint64Type(), m_loc);

			auto slotIdxCmp = awst::makeIntegerConstant(slot, m_loc);

			auto slotPresent = awst::makeNumericCompare(std::move(numArgsCheck), awst::NumericComparison::Gt, std::move(slotIdxCmp), m_loc);

			std::shared_ptr<awst::Expression> slotBytes =
				awst::makeAppArg(slot, m_loc);
			if (slot == 0)
				slotBytes = builder::SelectorSemantics::runtimeSelector(
					m_ctx, std::move(slotBytes), m_loc);

			auto slotChoice = awst::makeConditional(
				std::move(slotPresent), std::move(slotBytes),
				awst::makeBzero(0, m_loc),
				awst::WType::bytesType(), m_loc);

			if (!calldataConcat)
			{
				calldataConcat = std::move(slotChoice);
			}
			else
				calldataConcat = awst::makeConcat(std::move(calldataConcat), std::move(slotChoice), m_loc);
		}

		return awst::makeConditional(
			std::move(hasData), std::move(calldataConcat),
			awst::makeBzero(0, m_loc),
			awst::WType::bytesType(), m_loc);
	}

	// Fall through to IntrinsicMapper for standard intrinsics (block.timestamp, etc.)
	auto intrinsic = builder::IntrinsicMapper::tryMapMemberAccess(baseName, member, m_loc);
	if (intrinsic)
	{
		auto* solType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		if (intrinsic->wtype == awst::WType::bytesType()
			&& solType == awst::WType::biguintType())
		{
			auto cast = awst::makeAsBiguint(std::move(intrinsic), m_loc);
			return cast;
		}
		return intrinsic;
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast
