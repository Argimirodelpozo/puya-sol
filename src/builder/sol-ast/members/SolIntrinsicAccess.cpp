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

	// block.difficulty → 0 (no PoW on Algorand)
	if (baseName == "block" && member == "difficulty")
	{
		Logger::instance().warning(
			"block.difficulty returns 0 on AVM — Algorand has no proof-of-work.", m_loc);
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = m_loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";
		return zero;
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

		auto two = std::make_shared<awst::IntegerConstant>();
		two->sourceLocation = m_loc;
		two->wtype = awst::WType::uint64Type();
		two->value = "2";

		auto prevRound = std::make_shared<awst::UInt64BinaryOperation>();
		prevRound->sourceLocation = m_loc;
		prevRound->wtype = awst::WType::uint64Type();
		prevRound->left = std::move(round);
		prevRound->op = awst::UInt64BinaryOperator::Sub;
		prevRound->right = std::move(two);

		auto blockSeed = std::make_shared<awst::IntrinsicCall>();
		blockSeed->sourceLocation = m_loc;
		blockSeed->wtype = awst::WType::bytesType();
		blockSeed->opCode = "block";
		blockSeed->immediates = {std::string("BlkSeed")};
		blockSeed->stackArgs.push_back(std::move(prevRound));

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(blockSeed);
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

		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = m_loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";
		auto hasPayment = std::make_shared<awst::NumericComparisonExpression>();
		hasPayment->sourceLocation = m_loc;
		hasPayment->wtype = awst::WType::boolType();
		hasPayment->lhs = groupIdx;
		hasPayment->op = awst::NumericComparison::Gt;
		hasPayment->rhs = std::move(zero);

		auto groupIdx2 = std::make_shared<awst::IntrinsicCall>();
		groupIdx2->sourceLocation = m_loc;
		groupIdx2->wtype = awst::WType::uint64Type();
		groupIdx2->opCode = "txn";
		groupIdx2->immediates = {std::string("GroupIndex")};
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = m_loc;
		one->wtype = awst::WType::uint64Type();
		one->value = "1";
		auto payIdx = std::make_shared<awst::UInt64BinaryOperation>();
		payIdx->sourceLocation = m_loc;
		payIdx->wtype = awst::WType::uint64Type();
		payIdx->left = std::move(groupIdx2);
		payIdx->op = awst::UInt64BinaryOperator::Sub;
		payIdx->right = std::move(one);

		auto amount = std::make_shared<awst::IntrinsicCall>();
		amount->sourceLocation = m_loc;
		amount->wtype = awst::WType::uint64Type();
		amount->opCode = "gtxns";
		amount->immediates = {std::string("Amount")};
		amount->stackArgs.push_back(std::move(payIdx));

		auto zeroVal = std::make_shared<awst::IntegerConstant>();
		zeroVal->sourceLocation = m_loc;
		zeroVal->wtype = awst::WType::uint64Type();
		zeroVal->value = "0";

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
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(itob);
		return cast;
	}

	// msg.data → conditional: NumAppArgs > 0 ? ApplicationArgs[0] : bzero(0)
	// Handles bare calls where no ApplicationArgs are provided.
	if (baseName == "msg" && member == "data")
	{
		auto numAppArgs = std::make_shared<awst::IntrinsicCall>();
		numAppArgs->sourceLocation = m_loc;
		numAppArgs->wtype = awst::WType::uint64Type();
		numAppArgs->opCode = "txn";
		numAppArgs->immediates = {std::string("NumAppArgs")};

		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = m_loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";

		auto hasData = std::make_shared<awst::NumericComparisonExpression>();
		hasData->sourceLocation = m_loc;
		hasData->wtype = awst::WType::boolType();
		hasData->lhs = std::move(numAppArgs);
		hasData->op = awst::NumericComparison::Gt;
		hasData->rhs = std::move(zero);

		auto appArgs0 = std::make_shared<awst::IntrinsicCall>();
		appArgs0->sourceLocation = m_loc;
		appArgs0->wtype = awst::WType::bytesType();
		appArgs0->opCode = "txna";
		appArgs0->immediates = {std::string("ApplicationArgs"), 0};

		auto bzeroSize = std::make_shared<awst::IntegerConstant>();
		bzeroSize->sourceLocation = m_loc;
		bzeroSize->wtype = awst::WType::uint64Type();
		bzeroSize->value = "0";

		auto emptyBytes = std::make_shared<awst::IntrinsicCall>();
		emptyBytes->sourceLocation = m_loc;
		emptyBytes->wtype = awst::WType::bytesType();
		emptyBytes->opCode = "bzero";
		emptyBytes->stackArgs.push_back(std::move(bzeroSize));

		auto cond = std::make_shared<awst::ConditionalExpression>();
		cond->sourceLocation = m_loc;
		cond->wtype = awst::WType::bytesType();
		cond->condition = std::move(hasData);
		cond->trueExpr = std::move(appArgs0);
		cond->falseExpr = std::move(emptyBytes);
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
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = m_loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(intrinsic);
			return cast;
		}
		return intrinsic;
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast
