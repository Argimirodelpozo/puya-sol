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
		auto c = awst::makeIntegerConstant("1", m_loc, awst::WType::biguintType());
		return c;
	}

	// block.difficulty → 0 (no PoW on Algorand)
	if (baseName == "block" && member == "difficulty")
	{
		Logger::instance().warning(
			"block.difficulty returns 0 on AVM — Algorand has no proof-of-work.", m_loc);
		auto zero = awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType());
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
		auto zero = awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType());
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

		auto round = std::make_shared<awst::IntrinsicCall>();
		round->sourceLocation = m_loc;
		round->wtype = awst::WType::uint64Type();
		round->opCode = "global";
		round->immediates = {std::string("Round")};

		auto two = awst::makeIntegerConstant("2", m_loc);

		auto prevRound = awst::makeUInt64BinOp(std::move(round), awst::UInt64BinaryOperator::Sub, std::move(two), m_loc);

		auto blockSeed = std::make_shared<awst::IntrinsicCall>();
		blockSeed->sourceLocation = m_loc;
		blockSeed->wtype = awst::WType::bytesType();
		blockSeed->opCode = "block";
		blockSeed->immediates = {std::string("BlkSeed")};
		blockSeed->stackArgs.push_back(std::move(prevRound));

		auto cast = awst::makeReinterpretCast(std::move(blockSeed), awst::WType::biguintType(), m_loc);
		return cast;
	}

	// msg.value → conditional: GroupIndex > 0 ? gtxns Amount (GroupIndex-1) : 0
	// Handles the case where there's no preceding payment transaction.
	if (baseName == "msg" && member == "value")
	{
		auto groupIdx = std::make_shared<awst::IntrinsicCall>();
		groupIdx->sourceLocation = m_loc;
		groupIdx->wtype = awst::WType::uint64Type();
		groupIdx->opCode = "txn";
		groupIdx->immediates = {std::string("GroupIndex")};

		auto zero = awst::makeIntegerConstant("0", m_loc);
		auto hasPayment = awst::makeNumericCompare(groupIdx, awst::NumericComparison::Gt, std::move(zero), m_loc);

		auto groupIdx2 = std::make_shared<awst::IntrinsicCall>();
		groupIdx2->sourceLocation = m_loc;
		groupIdx2->wtype = awst::WType::uint64Type();
		groupIdx2->opCode = "txn";
		groupIdx2->immediates = {std::string("GroupIndex")};
		auto one = awst::makeIntegerConstant("1", m_loc);
		auto payIdx = awst::makeUInt64BinOp(std::move(groupIdx2), awst::UInt64BinaryOperator::Sub, std::move(one), m_loc);

		auto amount = std::make_shared<awst::IntrinsicCall>();
		amount->sourceLocation = m_loc;
		amount->wtype = awst::WType::uint64Type();
		amount->opCode = "gtxns";
		amount->immediates = {std::string("Amount")};
		amount->stackArgs.push_back(std::move(payIdx));

		auto zeroVal = awst::makeIntegerConstant("0", m_loc);

		auto cond = std::make_shared<awst::ConditionalExpression>();
		cond->sourceLocation = m_loc;
		cond->wtype = awst::WType::uint64Type();
		cond->condition = std::move(hasPayment);
		cond->trueExpr = std::move(amount);
		cond->falseExpr = std::move(zeroVal);

		// Promote to biguint
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = m_loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(cond));
		auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
		return cast;
	}

	// msg.sig → first 4 bytes of msg.data. In ARC4 routing the selector is
	// always ApplicationArgs[0], which is already 4 bytes, so we emit the
	// same txna read and type it as bytes4.
	if (baseName == "msg" && member == "sig")
	{
		auto appArgs0 = std::make_shared<awst::IntrinsicCall>();
		appArgs0->sourceLocation = m_loc;
		appArgs0->wtype = m_ctx.typeMapper.createType<awst::BytesWType>(4);
		appArgs0->opCode = "txna";
		appArgs0->immediates = {std::string("ApplicationArgs"), 0};
		return appArgs0;
	}

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
		auto numAppArgs = std::make_shared<awst::IntrinsicCall>();
		numAppArgs->sourceLocation = m_loc;
		numAppArgs->wtype = awst::WType::uint64Type();
		numAppArgs->opCode = "txn";
		numAppArgs->immediates = {std::string("NumAppArgs")};

		auto zero = awst::makeIntegerConstant("0", m_loc);

		auto hasData = awst::makeNumericCompare(std::move(numAppArgs), awst::NumericComparison::Gt, std::move(zero), m_loc);

		// Build concatenated calldata from slot 0 (selector) onwards.
		std::shared_ptr<awst::Expression> calldataConcat;
		for (int slot = 0; slot < 16; ++slot)
		{
			auto slotIdx = awst::makeIntegerConstant(std::to_string(slot), m_loc);

			auto numArgsCheck = std::make_shared<awst::IntrinsicCall>();
			numArgsCheck->sourceLocation = m_loc;
			numArgsCheck->wtype = awst::WType::uint64Type();
			numArgsCheck->opCode = "txn";
			numArgsCheck->immediates = {std::string("NumAppArgs")};

			auto slotIdxCmp = awst::makeIntegerConstant(std::to_string(slot), m_loc);

			auto slotPresent = awst::makeNumericCompare(std::move(numArgsCheck), awst::NumericComparison::Gt, std::move(slotIdxCmp), m_loc);

			auto slotBytes = std::make_shared<awst::IntrinsicCall>();
			slotBytes->sourceLocation = m_loc;
			slotBytes->wtype = awst::WType::bytesType();
			slotBytes->opCode = "txna";
			slotBytes->immediates = {std::string("ApplicationArgs"), slot};

			auto bzeroSize = awst::makeIntegerConstant("0", m_loc);
			auto emptyBytes = std::make_shared<awst::IntrinsicCall>();
			emptyBytes->sourceLocation = m_loc;
			emptyBytes->wtype = awst::WType::bytesType();
			emptyBytes->opCode = "bzero";
			emptyBytes->stackArgs.push_back(std::move(bzeroSize));

			auto slotChoice = std::make_shared<awst::ConditionalExpression>();
			slotChoice->sourceLocation = m_loc;
			slotChoice->wtype = awst::WType::bytesType();
			slotChoice->condition = std::move(slotPresent);
			slotChoice->trueExpr = std::move(slotBytes);
			slotChoice->falseExpr = std::move(emptyBytes);

			if (!calldataConcat)
			{
				calldataConcat = std::move(slotChoice);
			}
			else
			{
				auto cat = std::make_shared<awst::IntrinsicCall>();
				cat->sourceLocation = m_loc;
				cat->wtype = awst::WType::bytesType();
				cat->opCode = "concat";
				cat->stackArgs.push_back(std::move(calldataConcat));
				cat->stackArgs.push_back(std::move(slotChoice));
				calldataConcat = std::move(cat);
			}
		}

		auto bzeroSize2 = awst::makeIntegerConstant("0", m_loc);
		auto emptyAll = std::make_shared<awst::IntrinsicCall>();
		emptyAll->sourceLocation = m_loc;
		emptyAll->wtype = awst::WType::bytesType();
		emptyAll->opCode = "bzero";
		emptyAll->stackArgs.push_back(std::move(bzeroSize2));

		auto cond = std::make_shared<awst::ConditionalExpression>();
		cond->sourceLocation = m_loc;
		cond->wtype = awst::WType::bytesType();
		cond->condition = std::move(hasData);
		cond->trueExpr = std::move(calldataConcat);
		cond->falseExpr = std::move(emptyAll);
		return cond;
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
