/// @file SolTupleExpression.cpp — tuple/inline-array expression translation.

#include "builder/ast/exprs/SolTupleExpression.h"
#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/ast/calls/SolInternalCall.h"
#include "builder/solc/SolcFacts.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/eb/AssignmentHelper.h"
#include "builder/eb/AssemblyBoundary.h"
#include "builder/eb/CalldataReference.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/ConversionPlan.h"
#include "builder/types/EncodedSize.h"
#include "builder/codec/EvmMemoryCodec.h"
#include "awst/TupleValue.h"
#include <algorithm>
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::sol_ast
{

SolTupleExpression::SolTupleExpression(
	eb::ContractContext& _ctx,
	solidity::frontend::TupleExpression const& _node)
	: SolExpression(_ctx, _node), m_tuple(_node)
{
}

std::shared_ptr<awst::Expression> SolTupleExpression::toAwst()
{
	if (m_tuple.isInlineArray())
	{
		auto const* solType = dynamic_cast<solidity::frontend::ArrayType const*>(m_tuple.annotation().type);
		assert(solType);
		auto const* wtype = m_ctx.typeMapper.map(solType);
		auto const* elementType = awst::arrayElementType(wtype);
		auto const* nativeElement = m_ctx.typeMapper.map(solType->baseType());
		auto result = awst::makeNewArray(wtype, m_loc);
		for (auto const& component: m_tuple.components())
		{
			assert(component);
			auto lowered = m_ctx.lowerOperand([&] {
				auto value = buildExpr(*component);
				value = ConversionPlan{component->annotation().type, solType->baseType(),
					nativeElement, ConversionPlan::Context::Initialization}.emit(
						std::move(value), m_loc, &m_ctx.preEffects());
				return eb::AssignmentHelper::arc4EncodeForType(
					m_ctx, std::move(value), elementType, m_loc);
			}, false);
			// Solc evaluates array elements in source order. Finish each value and
			// its write-backs before a later element can change what it reads.
			result->values.push_back(m_ctx.emitSequencedOperand(
				std::move(lowered.effects), std::move(lowered.value), true, m_loc));
		}
		return result;
	}

	// Single-element tuple is parenthesization
	if (m_tuple.components().size() == 1 && m_tuple.components()[0])
		return buildExpr(*m_tuple.components()[0]);
	return buildTuple({});
}

std::shared_ptr<awst::Expression> SolTupleExpression::buildBindingRhs(
	eb::ContractContext& ctx, solidity::frontend::Expression const& expression,
	std::vector<solidity::frontend::VariableDeclaration const*> const& bindings)
{
	auto const& value = SolcFacts::functionExpression(expression);
	if (auto const* conditional = SolcFacts::expressionAs<solidity::frontend::Conditional>(&value))
	{
		auto const* tuple = dynamic_cast<solidity::frontend::TupleType const*>(value.annotation().type);
		if (!tuple || std::none_of(tuple->components().begin(), tuple->components().end(), [](auto const* type) {
			return type && type->dataStoredIn(solidity::frontend::DataLocation::Memory);
		})) return ctx.buildExpr(expression);
		auto loc = ctx.makeLoc(value.location());
		auto condition = ctx.emitSequencedOperand({}, ctx.pinIfWriteBacks(ctx.lower(conditional->condition(), false), loc), true, loc);
		auto yes = ctx.lowerOperand([&] { return buildBindingRhs(ctx, conditional->trueExpression(), bindings); });
		auto no = ctx.lowerOperand([&] { return buildBindingRhs(ctx, conditional->falseExpression(), bindings); });
		auto const* yesType = dynamic_cast<awst::WTuple const*>(yes.value->wtype);
		auto const* noType = dynamic_cast<awst::WTuple const*>(no.value->wtype);
		std::vector<awst::WType const*> types;
		for (size_t i = 0; i < tuple->components().size(); ++i)
		{
			auto const* type = tuple->components()[i];
			bool const pointer = type->dataStoredIn(solidity::frontend::DataLocation::Memory)
				&& (yesType->types()[i] == awst::WType::uint64Type() || noType->types()[i] == awst::WType::uint64Type());
			types.push_back(pointer ? awst::WType::uint64Type()
				: type->dataStoredIn(solidity::frontend::DataLocation::Storage) ? yesType->types()[i] : ctx.typeMapper.map(type));
		}
		auto const* resultType = ctx.typeMapper.createType<awst::WTuple>(std::move(types));
		auto adapt = [&](auto& branch, solidity::frontend::Expression const& source) {
			auto items = awst::tupleItems(std::move(branch.value), loc, &branch.effects.pre);
			auto const* sourceType = dynamic_cast<solidity::frontend::TupleType const*>(source.annotation().type);
			auto result = awst::makeTupleExpression(resultType, loc);
			for (size_t i = 0; i < items.size(); ++i)
			{
				auto const* type = tuple->components()[i];
				if (type->dataStoredIn(solidity::frontend::DataLocation::Memory)
					&& resultType->types()[i] == awst::WType::uint64Type() && items[i]->wtype != awst::WType::uint64Type())
				{
					auto id = awst::NameGen::next("SolTupleExpression.reference");
					auto name = "__tuple_reference_" + std::to_string(id);
					auto const* native = ctx.typeMapper.map(type);
					items[i] = ConversionPlan{sourceType->components()[i], type, native, ConversionPlan::Context::Initialization}.emit(std::move(items[i]), loc, &branch.effects.pre);
					if (!spillEvmMemoryValue(ctx.typeMapper, type, native, std::move(items[i]), name, id, loc, branch.effects.pre))
						throw SizeError("Cannot preserve conditional tuple reference");
					items[i] = awst::makeVarExpression(name, awst::WType::uint64Type(), loc);
				}
				result->items.push_back(ConversionPlan{sourceType->components()[i], type, resultType->types()[i],
					ConversionPlan::Context::Initialization}.emit(std::move(items[i]), loc, &branch.effects.pre));
			}
			branch.value = std::move(result);
		};
		adapt(yes, conditional->trueExpression());
		adapt(no, conditional->falseExpression());
		return ctx.emitConditional(std::move(condition), std::move(yes), std::move(no), resultType, loc);
	}
	if (auto const* call = SolcFacts::expressionAs<solidity::frontend::FunctionCall>(&value);
		call && SolInternalCall::hasMemoryReturns(*call))
		return SolInternalCall(ctx, *call).toReferenceAwst();
	if (auto const* tuple = SolcFacts::expressionAs<solidity::frontend::TupleExpression>(&value);
		tuple && !tuple->isInlineArray())
		return SolTupleExpression(ctx, *tuple).buildTuple(bindings, true);
	return ctx.buildExpr(expression);
}

std::shared_ptr<awst::Expression> SolTupleExpression::buildTuple(
	std::vector<solidity::frontend::VariableDeclaration const*> const& bindings,
	bool storageReferences)
{
	// Missing LHS components are represented by empty-name placeholders.
	auto e = awst::makeTupleExpression(nullptr, m_loc);
	std::vector<awst::WType const*> types;
	for (size_t i = 0; i < m_tuple.components().size(); ++i)
	{
		auto const& comp = m_tuple.components()[i];
		auto const* target = i < bindings.size() ? bindings[i] : nullptr;
		std::shared_ptr<awst::Expression> value;
		if (comp && target) value = assemblyScalarCopy(m_ctx, *target, *comp, m_loc);
		if (comp && target && target->referenceLocation() == solidity::frontend::VariableDeclaration::Location::CallData)
			if (auto reference = CalldataReference::resolve(m_ctx, *comp, m_loc)) value = reference->pack(m_loc);
		if (comp && storageReferences)
		{
			auto const& source = SolcFacts::unparenthesized(*comp);
			if (auto const* nested = SolcFacts::expressionAs<solidity::frontend::TupleExpression>(&source);
				nested && !nested->isInlineArray())
				value = SolTupleExpression(m_ctx, *nested).buildTuple({}, true);
			else if (!source.annotation().type->isValueType()
				&& ((m_ctx.typeMapper.profile().evmStorageLayout && EvmSlotLowering::isStorageStateRef(source))
					|| EvmSlotLowering::isSlotHandleRef(source, m_ctx, m_scope)))
			{
				// Solc tuples carry storage references, not aggregate snapshots.
				// Capture the address now; copy the value at its eventual store.
				auto address = EvmSlotLowering(m_ctx, m_scope, m_loc).resolve(source);
				if (!address) throw std::runtime_error("Cannot resolve tuple storage reference");
				value = m_ctx.emitSequencedOperand({}, address->slot, true, m_loc);
			}
		}
		if ((storageReferences || (target && target->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory))
			&& comp && comp->annotation().type
			&& comp->annotation().type->dataStoredIn(solidity::frontend::DataLocation::Memory)
			&& !comp->annotation().type->isValueType())
			if (auto reference = SolIndexAccess::resolveBlobReference(m_ctx, m_scope, *comp, m_loc))
				value = m_ctx.emitSequencedOperand(std::move(reference->effects),
					std::move(reference->value), true, m_loc);
		if (!value) value = comp ? buildExpr(*comp)
			: awst::makeVarExpression("", awst::WType::uint64Type(), m_loc);
		types.push_back(value->wtype);
		e->items.push_back(std::move(value));
	}
	e->wtype = m_ctx.typeMapper.createType<awst::WTuple>(std::move(types), std::nullopt);
	return e;
}

} // namespace puyasol::builder::sol_ast
