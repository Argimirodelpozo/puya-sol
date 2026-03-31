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
