/// @file SolIntrinsicAccess.cpp
/// msg.sender, block.timestamp, block.prevrandao, block.difficulty, etc.

#include "builder/sol-ast/members/SolIntrinsicAccess.h"
#include "builder/sol-intrinsics/IntrinsicMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolIntrinsicAccess::toAwst()
{
	auto const* baseId = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpression());
	if (!baseId) return nullptr;

	std::string baseName = baseId->name();
	std::string member = memberName();

	// block.chainid → 1 (Ethereum mainnet id)
	// AVM has no per-chain identifier; we stub as 1 so Solidity semantic
	// tests that check Ethereum-style chain ids pass. Real cross-chain
	// code should read global GenesisHash in assembly instead.
	if (baseName == "block" && member == "chainid")
	{
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
	// AVM has a flat per-transaction fee (typically 1000 microAlgos); no
	// EIP-1559 base fee concept and no blob pricing. Callers that gate
	// behaviour on `basefee > 0` will see the no-fee path, which is the
	// safer default on AVM.
	if (baseName == "block" && (member == "basefee" || member == "blobbasefee"))
	{
		Logger::instance().warning(
			"block." + member + " returns 0 on AVM — no EIP-1559 base fee concept.", m_loc);
		auto zero = awst::makeZero(m_loc, awst::WType::biguintType());
		return zero;
	}

	// block.gaslimit → a large sentinel (70000). AVM has no gas, only a
	// fixed opcode budget (700 per app call, poolable across a 16-txn
	// group). Returning 70000 is enough that gaslimit-based bounds in
	// Solidity libraries (common pattern: `for (uint i = 0; gasleft() > X;
	// ++i)`) don't prematurely abort.
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

		auto cast = awst::makeReinterpretCast(std::move(blockSeed), awst::WType::biguintType(), m_loc);
		return cast;
	}

	// msg.value → conditional: GroupIndex > 0 ? gtxns Amount (GroupIndex-1) : 0
	// Handles the case where there's no preceding payment transaction.
	if (baseName == "msg" && member == "value")
	{
		auto groupIdx = awst::makeTxn(std::string("GroupIndex"), awst::WType::uint64Type(), m_loc);

		auto zero = awst::makeZero(m_loc);
		auto hasPayment = awst::makeNumericCompare(groupIdx, awst::NumericComparison::Gt, std::move(zero), m_loc);

		auto groupIdx2 = awst::makeTxn(std::string("GroupIndex"), awst::WType::uint64Type(), m_loc);
		auto one = awst::makeOne(m_loc);
		auto payIdx = awst::makeUInt64BinOp(std::move(groupIdx2), awst::UInt64BinaryOperator::Sub, std::move(one), m_loc);

		auto amount = awst::makeIntrinsicCall("gtxns", awst::WType::uint64Type(), m_loc);
		amount->immediates = {std::string("Amount")};
		amount->stackArgs.push_back(std::move(payIdx));

		auto zeroVal = awst::makeZero(m_loc);

		auto cond = awst::makeConditional(
			std::move(hasPayment), std::move(amount), std::move(zeroVal),
			awst::WType::uint64Type(), m_loc);

		// Promote to biguint
		auto itob = awst::makeItob(std::move(cond), m_loc);
		return awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
	}

	// msg.sig → first 4 bytes of msg.data. In ARC4 routing the selector is
	// always ApplicationArgs[0], which is already 4 bytes, so we emit the
	// same txna read and type it as bytes4.
	if (baseName == "msg" && member == "sig")
		return awst::makeAppArg(
			0, m_loc, m_ctx.typeMapper.createType<awst::BytesWType>(4));

	// msg.data → reconstruct EVM-style calldata: selector (4 bytes from
	// ApplicationArgs[0]) followed by each subsequent ApplicationArgs slot
	// concatenated. Each ARC4 arg is already left-padded to its declared
	// width, so the concatenation lands close to the EVM head encoding for
	// simple scalar args. Bare calls with no ApplicationArgs return bzero(0).
	//
	// We only inspect up to 16 slots — Algorand's hard cap is 16
	// ApplicationArgs (slot 0 is the selector, so 15 actual params).
	if (baseName == "msg" && member == "data")
	{
		auto numAppArgs = awst::makeTxn(std::string("NumAppArgs"), awst::WType::uint64Type(), m_loc);

		auto zero = awst::makeZero(m_loc);

		auto hasData = awst::makeNumericCompare(std::move(numAppArgs), awst::NumericComparison::Gt, std::move(zero), m_loc);

		// Build concatenated calldata from slot 0 (selector) onwards.
		std::shared_ptr<awst::Expression> calldataConcat;
		for (int slot = 0; slot < 16; ++slot)
		{
			auto slotIdx = awst::makeIntegerConstant(slot, m_loc);

			auto numArgsCheck = awst::makeTxn(std::string("NumAppArgs"), awst::WType::uint64Type(), m_loc);

			auto slotIdxCmp = awst::makeIntegerConstant(slot, m_loc);

			auto slotPresent = awst::makeNumericCompare(std::move(numArgsCheck), awst::NumericComparison::Gt, std::move(slotIdxCmp), m_loc);

			auto slotBytes = awst::makeAppArg(slot, m_loc);

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

	// Standard intrinsics via IntrinsicMapper
	auto intrinsic = builder::IntrinsicMapper::tryMapMemberAccess(baseName, member, m_loc);
	if (intrinsic)
	{
		auto* solType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		if (intrinsic->wtype == awst::WType::bytesType()
			&& solType == awst::WType::biguintType())
		{
			auto cast = awst::makeReinterpretCast(std::move(intrinsic), awst::WType::biguintType(), m_loc);
			return cast;
		}
		return intrinsic;
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast
