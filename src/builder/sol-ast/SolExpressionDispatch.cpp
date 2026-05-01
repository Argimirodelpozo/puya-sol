/// @file SolExpressionDispatch.cpp
/// Central expression dispatcher — implemented as a SolASTVisitor subclass.
///
/// `buildExpression` constructs a per-call SolExpressionVisitor, hands it the
/// translation context, and dispatches by Solidity AST kind to the right
/// per-kind handler in src/builder/sol-ast/exprs/. The visitor base
/// (SolASTVisitor.h) handles the dynamic_cast cascade.

#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/sol-ast/SolASTVisitor.h"
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

namespace
{

class SolExpressionVisitor: public SolASTVisitor<std::shared_ptr<awst::Expression>>
{
public:
	explicit SolExpressionVisitor(eb::BuilderContext& _ctx): m_ctx(_ctx) {}

	std::shared_ptr<awst::Expression> visitLiteral(Literal const& _n) override
	{
		SolLiteral handler(m_ctx, _n);
		return handler.toAwst();
	}

	std::shared_ptr<awst::Expression> visitIdentifier(Identifier const& _n) override
	{
		SolIdentifier handler(m_ctx, _n);
		return handler.toAwst();
	}

	std::shared_ptr<awst::Expression> visitBinaryOp(BinaryOperation const& _n) override
	{
		SolBinaryOperation handler(m_ctx, _n);
		return handler.toAwst();
	}

	std::shared_ptr<awst::Expression> visitUnaryOp(UnaryOperation const& _n) override
	{
		SolUnaryOperation handler(m_ctx, _n);
		return handler.toAwst();
	}

	std::shared_ptr<awst::Expression> visitConditional(Conditional const& _n) override
	{
		SolConditional handler(m_ctx, _n);
		return handler.toAwst();
	}

	std::shared_ptr<awst::Expression> visitAssignment(Assignment const& _n) override
	{
		SolAssignment handler(m_ctx, _n);
		return handler.toAwst();
	}

	std::shared_ptr<awst::Expression> visitIndexAccess(IndexAccess const& _n) override
	{
		SolIndexAccess handler(m_ctx, _n);
		return handler.toAwst();
	}

	std::shared_ptr<awst::Expression> visitIndexRange(IndexRangeAccess const& _n) override
	{
		SolIndexRangeAccess handler(m_ctx, _n);
		return handler.toAwst();
	}

	std::shared_ptr<awst::Expression> visitTuple(TupleExpression const& _n) override
	{
		SolTupleExpression handler(m_ctx, _n);
		return handler.toAwst();
	}

	std::shared_ptr<awst::Expression> visitFunctionCall(FunctionCall const& _n) override
	{
		SolExpressionFactory factory(m_ctx);
		auto handler = factory.createFunctionCall(_n);
		if (handler)
			return handler->toAwst();
		Logger::instance().warning("unhandled function call kind", makeLoc(_n));
		return makeVoid(_n);
	}

	std::shared_ptr<awst::Expression> visitMemberAccess(MemberAccess const& _n) override
	{
		SolExpressionFactory factory(m_ctx);
		auto handler = factory.createMemberAccess(_n);
		if (handler)
		{
			auto result = handler->toAwst();
			if (result) return result;
		}

		// Fallback: sol-eb builder dispatch on the base value's instance builder.
		auto base = visit(_n.expression());
		auto loc = makeLoc(_n);
		auto* baseSolType = _n.expression().annotation().type;
		auto builder = m_ctx.builderForInstance(baseSolType, base);
		if (builder)
		{
			auto result = builder->member_access(_n.memberName(), loc);
			if (result)
			{
				if (auto* instBuilder = dynamic_cast<eb::InstanceBuilder*>(result.get()))
					return instBuilder->resolve();
			}
		}

		// Ultimate fallback — emit a typed zero of the expected result type.
		Logger::instance().warning(
			"unsupported member access '." + _n.memberName() + "'", loc);
		auto* wtype = m_ctx.typeMapper.map(_n.annotation().type);
		if (wtype == awst::WType::uint64Type() || wtype == awst::WType::biguintType())
			return awst::makeIntegerConstant("0", loc, wtype);
		if (wtype == awst::WType::boolType())
			return awst::makeBoolConstant(false, loc, wtype);
		return awst::makeBytesConstant({}, loc);
	}

	std::shared_ptr<awst::Expression> visitCallOptions(FunctionCallOptions const& _n) override
	{
		// {value:, gas:} options are consumed by the outer FunctionCall via
		// SolFunctionCall::extractCallValue. Here we just translate the base.
		Logger::instance().warning(
			"function call options {value:, gas:} ignored on Algorand", makeLoc(_n));
		return visit(_n.expression());
	}

	std::shared_ptr<awst::Expression> visitTypeName(ElementaryTypeNameExpression const& _n) override
	{
		return makeVoid(_n);
	}

	std::shared_ptr<awst::Expression> visitDefault(solidity::frontend::ASTNode const& _node) override
	{
		Logger::instance().warning("unhandled expression type", makeLoc(_node));
		return makeVoid(_node);
	}

private:
	eb::BuilderContext& m_ctx;

	awst::SourceLocation makeLoc(solidity::frontend::ASTNode const& _node)
	{
		auto const& l = _node.location();
		return m_ctx.makeLoc(l.start, l.end);
	}

	std::shared_ptr<awst::Expression> makeVoid(solidity::frontend::ASTNode const& _node)
	{
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = makeLoc(_node);
		vc->wtype = awst::WType::voidType();
		return vc;
	}
};

} // anonymous namespace

std::shared_ptr<awst::Expression> buildExpression(
	eb::BuilderContext& _ctx,
	Expression const& _expr)
{
	SolExpressionVisitor visitor(_ctx);
	return visitor.visit(_expr);
}

} // namespace puyasol::builder::sol_ast
