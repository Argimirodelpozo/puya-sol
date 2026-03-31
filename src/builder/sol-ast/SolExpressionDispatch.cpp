/// @file SolExpressionDispatch.cpp
/// Central expression dispatcher — replaces ExpressionBuilder's visitor pattern.

#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/sol-ast/SolExpressionFactory.h"
#include "builder/sol-ast/exprs/SolLiteral.h"
#include "builder/sol-ast/exprs/SolConditional.h"
#include "builder/sol-ast/exprs/SolIdentifier.h"
#include "builder/sol-ast/exprs/SolTupleExpression.h"
#include "builder/sol-ast/exprs/SolBinaryOperation.h"
#include "builder/sol-ast/exprs/SolUnaryOperation.h"
#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> buildExpression(
	eb::BuilderContext& _ctx,
	Expression const& _expr)
{
	auto makeLoc = [&](Expression const& e) {
		return _ctx.makeLoc(e.location().start, e.location().end);
	};

	// Literal
	if (auto const* node = dynamic_cast<Literal const*>(&_expr))
	{
		SolLiteral handler(_ctx, *node);
		return handler.toAwst();
	}

	// Identifier
	if (auto const* node = dynamic_cast<Identifier const*>(&_expr))
	{
		SolIdentifier handler(_ctx, *node);
		return handler.toAwst();
	}

	// BinaryOperation
	if (auto const* node = dynamic_cast<BinaryOperation const*>(&_expr))
	{
		SolBinaryOperation handler(_ctx, *node);
		return handler.toAwst();
	}

	// UnaryOperation
	if (auto const* node = dynamic_cast<UnaryOperation const*>(&_expr))
	{
		SolUnaryOperation handler(_ctx, *node);
		return handler.toAwst();
	}

	// FunctionCall — uses factory for Kind dispatch
	if (auto const* node = dynamic_cast<FunctionCall const*>(&_expr))
	{
		SolExpressionFactory factory(_ctx);
		auto handler = factory.createFunctionCall(*node);
		if (handler)
			return handler->toAwst();
		// Unhandled function call kind
		Logger::instance().warning("unhandled function call kind", makeLoc(*node));
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = makeLoc(*node);
		vc->wtype = awst::WType::voidType();
		return vc;
	}

	// MemberAccess — uses factory for dispatch
	if (auto const* node = dynamic_cast<MemberAccess const*>(&_expr))
	{
		SolExpressionFactory factory(_ctx);
		auto handler = factory.createMemberAccess(*node);
		if (handler)
		{
			auto result = handler->toAwst();
			if (result) return result;
		}

		// Fallback: sol-eb builder dispatch
		auto base = buildExpression(_ctx, node->expression());
		auto loc = makeLoc(*node);
		auto* baseSolType = node->expression().annotation().type;
		auto builder = _ctx.builderForInstance(baseSolType, base);
		if (builder)
		{
			auto result = builder->member_access(node->memberName(), loc);
			if (result)
			{
				if (auto* instBuilder = dynamic_cast<eb::InstanceBuilder*>(result.get()))
					return instBuilder->resolve();
			}
		}

		// Ultimate fallback
		Logger::instance().warning(
			"unsupported member access '." + node->memberName() + "'", loc);
		auto* wtype = _ctx.typeMapper.map(node->annotation().type);
		if (wtype == awst::WType::uint64Type() || wtype == awst::WType::biguintType())
		{
			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = loc;
			e->wtype = wtype;
			e->value = "0";
			return e;
		}
		if (wtype == awst::WType::boolType())
		{
			auto e = std::make_shared<awst::BoolConstant>();
			e->sourceLocation = loc;
			e->wtype = wtype;
			e->value = false;
			return e;
		}
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::bytesType();
		e->encoding = awst::BytesEncoding::Base16;
		e->value = {};
		return e;
	}

	// Conditional
	if (auto const* node = dynamic_cast<Conditional const*>(&_expr))
	{
		SolConditional handler(_ctx, *node);
		return handler.toAwst();
	}

	// Assignment
	if (auto const* node = dynamic_cast<Assignment const*>(&_expr))
	{
		SolAssignment handler(_ctx, *node);
		return handler.toAwst();
	}

	// IndexAccess
	if (auto const* node = dynamic_cast<IndexAccess const*>(&_expr))
	{
		SolIndexAccess handler(_ctx, *node);
		return handler.toAwst();
	}

	// IndexRangeAccess
	if (auto const* node = dynamic_cast<IndexRangeAccess const*>(&_expr))
	{
		SolIndexRangeAccess handler(_ctx, *node);
		return handler.toAwst();
	}

	// TupleExpression
	if (auto const* node = dynamic_cast<TupleExpression const*>(&_expr))
	{
		SolTupleExpression handler(_ctx, *node);
		return handler.toAwst();
	}

	// FunctionCallOptions — payment inner transaction
	if (auto const* node = dynamic_cast<FunctionCallOptions const*>(&_expr))
	{
		// Translate the base expression (options like {value:, gas:} are handled
		// by SolFunctionCall::extractCallValue when the outer FunctionCall is processed)
		Logger::instance().warning(
			"function call options {value:, gas:} ignored on Algorand", makeLoc(*node));
		return buildExpression(_ctx, node->expression());
	}

	// ElementaryTypeNameExpression
	if (dynamic_cast<ElementaryTypeNameExpression const*>(&_expr))
	{
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = makeLoc(_expr);
		vc->wtype = awst::WType::voidType();
		return vc;
	}

	// Unknown expression type
	Logger::instance().warning("unhandled expression type", makeLoc(_expr));
	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = makeLoc(_expr);
	vc->wtype = awst::WType::voidType();
	return vc;
}

} // namespace puyasol::builder::sol_ast
