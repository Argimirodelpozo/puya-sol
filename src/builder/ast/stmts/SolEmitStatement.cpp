/// @file SolEmitStatement.cpp

#include "builder/ast/stmts/SolEmitStatement.h"
#include "builder/eb/CallOperands.h"
#include "builder/solc/SolcFacts.h"
#include "builder/context/ContractContext.h"
#include "builder/types/ConversionPlan.h"
#include "builder/types/TypeCoercion.h"
#include "builder/codec/SelectorSemantics.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>
#include <stdexcept>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolEmitStatement::SolEmitStatement(
	BlockContext& blk, EmitStatement const& node, awst::SourceLocation loc)
	: SolStatement(blk, std::move(loc)), m_node(node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolEmitStatement::toAwst()
{
	auto const& call = m_node.eventCall();
	auto const* event = dynamic_cast<EventDefinition const*>(
		ASTNode::referencedDeclaration(SolcFacts::functionExpression(call.expression())));
	if (!event) throw std::logic_error("Event has no resolved solc declaration");
	auto& ctx = m_blk.builderCtx();
	auto& types = m_blk.typeMapper();
	auto const* wire = SelectorSemantics::eventType(types, *event);
	auto values = CallOperands::build(ctx, call, m_loc, [&](Expression const& source, size_t i) {
		auto const* declared = event->parameters().at(i)->type();
		auto value = ConversionPlan{source.annotation().type, declared, types.map(declared),
			ConversionPlan::Context::Argument}.emit(ctx.buildExpr(source), m_loc, &ctx.preEffects());
		if (auto const* enumeration = dynamic_cast<EnumType const*>(declared))
		{
			value = ctx.emitSequencedOperand({}, std::move(value), true, m_loc);
			ctx.queuePreExpression(awst::makeEnumRangeAssert(
				TypeCoercion::coerceScalar(value, awst::WType::uint64Type(), m_loc),
				enumeration->numberOfMembers(), m_loc), m_loc);
		}
		return value;
	});

	std::shared_ptr<awst::Expression> emitted;
	if (values.empty())
	{
		auto log = awst::makeIntrinsicCall("log", awst::WType::voidType(), m_loc);
		log->stackArgs.push_back(awst::makeMethodConstant(event->name() + "()",
			awst::WType::bytesType(), m_loc));
		emitted = std::move(log);
	}
	else
	{
		// ARC-28 logs encode all declared fields. Indexed/dynamic arguments are
		// not EVM topics; this accepted divergence is independent of selector mode.
		auto value = awst::makeNewStruct(wire, m_loc);
		for (size_t i = 0; i < values.size(); ++i)
		{
			auto const& [name, type] = wire->fields()[i];
			// Evaluate all arguments before encoding mutable references. Native
			// scalars still need ARC4 encoding; assignment coercion is not a codec.
			value->values[name] = awst::structurallyEquivalent(values[i]->wtype, type)
				? std::move(values[i]) : awst::makeARC4Encode(std::move(values[i]), type, m_loc);
		}
		emitted = awst::makeEmit(std::move(value), m_loc);
	}
	std::vector<std::shared_ptr<awst::Statement>> result;
	ctx.appendEffectsTo(result);
	result.push_back(awst::makeExpressionStatement(std::move(emitted), m_loc));
	return result;
}

} // namespace puyasol::builder::sol_ast
