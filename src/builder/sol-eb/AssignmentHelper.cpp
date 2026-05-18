/// @file AssignmentHelper.cpp
/// Compound assignment via builder pattern + ARC4 struct chain rebuild.

#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-eb/BuilderRegistry.h"
#include "builder/storage/StorageMapper.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

std::shared_ptr<awst::Expression> AssignmentHelper::tryComputeCompoundValue(
	ContractContext& _ctx,
	solidity::frontend::Token _assignOp,
	solidity::frontend::Type const* _targetSolType,
	std::shared_ptr<awst::Expression> _currentValue,
	std::shared_ptr<awst::Expression> _rhs,
	awst::SourceLocation const& _loc)
{
	using Token = solidity::frontend::Token;

	// Map assign operator to binary op
	auto mapOp = [](Token t) -> std::optional<BuilderBinaryOp> {
		switch (t)
		{
		case Token::AssignAdd: return BuilderBinaryOp::Add;
		case Token::AssignSub: return BuilderBinaryOp::Sub;
		case Token::AssignMul: return BuilderBinaryOp::Mult;
		case Token::AssignDiv: return BuilderBinaryOp::Div;
		case Token::AssignMod: return BuilderBinaryOp::Mod;
		case Token::AssignShl: return BuilderBinaryOp::LShift;
		case Token::AssignShr: case Token::AssignSar: return BuilderBinaryOp::RShift;
		case Token::AssignBitOr: return BuilderBinaryOp::BitOr;
		case Token::AssignBitXor: return BuilderBinaryOp::BitXor;
		case Token::AssignBitAnd: return BuilderBinaryOp::BitAnd;
		default: return std::nullopt;
		}
	};

	auto binOp = mapOp(_assignOp);
	if (!binOp)
		return nullptr;

	if (!_targetSolType)
		return nullptr;

	// Create builders for both sides
	auto leftBuilder = _ctx.builderForInstance(_targetSolType, _currentValue);
	if (!leftBuilder)
		return nullptr;

	// For the RHS, use the same Solidity type — compound assignment operates on same type
	auto rightBuilder = _ctx.builderForInstance(_targetSolType, _rhs);
	if (!rightBuilder)
		return nullptr;

	auto result = leftBuilder->binary_op(*rightBuilder, *binOp, _loc);
	if (!result)
		return nullptr;

	return result->resolve();
}

ArcStructCowResult AssignmentHelper::rebuildArc4StructChainCOW(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _initialTarget,
	std::shared_ptr<awst::Expression> _initialValue,
	awst::SourceLocation const& _loc)
{
	ArcStructCowResult result;
	result.assignTarget = std::move(_initialTarget);
	result.assignValue = std::move(_initialValue);

	while (auto const* outerField =
		dynamic_cast<awst::FieldExpression const*>(result.assignTarget.get()))
	{
		// Outer base's struct type — direct, or peek through StateGet.
		auto const* outerStructType =
			dynamic_cast<awst::ARC4Struct const*>(outerField->base->wtype);
		if (!outerStructType)
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(outerField->base.get()))
				outerStructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
		if (!outerStructType) break;

		auto outerBase = outerField->base;
		// Write target: bare base, no StateGet wrapper (puya rejects).
		auto outerWriteBase = outerBase;
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(outerBase.get()))
			outerWriteBase = sg->field;
		// Read base: surviving fields need a read-shape (BoxValue needs to
		// be wrapped in StateGet so field reads return the box content).
		auto outerReadBase = outerBase;
		if (dynamic_cast<awst::BoxValueExpression const*>(outerWriteBase.get())
			&& !dynamic_cast<awst::StateGet const*>(outerBase.get()))
			outerReadBase = builder::StorageMapper::makeStateGetWithDefault(
				outerWriteBase, outerWriteBase->wtype, _loc);

		std::string outerFieldName = outerField->name;
		awst::WType const* outerFieldWtype = nullptr;
		for (auto const& [fn, ft]: outerStructType->fields())
			if (fn == outerFieldName) { outerFieldWtype = ft; break; }
		result.fieldChain.push_back({outerFieldName, outerFieldWtype});

		auto outerNewStruct = awst::makeNewStruct(outerStructType, _loc);
		for (auto const& [fn, ft]: outerStructType->fields())
		{
			if (fn == outerFieldName)
				outerNewStruct->values[fn] = std::move(result.assignValue);
			else
				outerNewStruct->values[fn] =
					awst::makeFieldExpression(outerReadBase, fn, ft, _loc);
		}
		result.assignTarget = std::move(outerWriteBase);
		result.assignValue = std::move(outerNewStruct);
	}

	return result;
}

} // namespace puyasol::builder::eb
