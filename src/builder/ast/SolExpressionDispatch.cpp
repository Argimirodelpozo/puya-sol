/// @file SolExpressionDispatch.cpp
/// Central expression dispatcher using solc's ASTConstVisitor. Handlers own
/// child lowering and evaluation order; returning false disables the child walk.

#include "builder/solc/SolcFacts.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/types/CallBoundaryPlan.h"
#include "builder/eb/CallOperands.h"
#include "builder/eb/ResolvedLValue.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/ast/SolExpressionDispatch.h"
#include "builder/ast/SolExpressionFactory.h"
#include "builder/ast/exprs/SolLiteral.h"
#include "builder/ast/exprs/SolConditional.h"
#include "builder/ast/exprs/SolIdentifier.h"
#include "builder/ast/exprs/SolTupleExpression.h"
#include "builder/ast/exprs/SolBinaryOperation.h"
#include "builder/ast/exprs/SolUnaryOperation.h"
#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/yul/AssemblyBuilder.h"
#include "builder/ast/exprs/SolAssignment.h"
#include "builder/eb/NodeBuilder.h"
#include "builder/types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <algorithm>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{

class SolExpressionVisitor: public ASTConstVisitor
{
public:
	explicit SolExpressionVisitor(eb::ContractContext& _ctx): m_ctx(_ctx) {}

	std::shared_ptr<awst::Expression> build(Expression const& _n)
	{
		SolcFacts::unparenthesized(_n).accept(*this);
		return std::move(m_result);
	}

	bool visit(Literal const& _n) override
	{
		m_result = SolLiteral(m_ctx, _n).toAwst();
		return false;
	}

	bool visit(Identifier const& _n) override
	{
		m_result = SolIdentifier(m_ctx, _n).toAwst();
		return false;
	}

	bool visit(BinaryOperation const& _n) override
	{
		m_result = SolBinaryOperation(m_ctx, _n).toAwst();
		return false;
	}

	bool visit(UnaryOperation const& _n) override
	{
		m_result = SolUnaryOperation(m_ctx, _n).toAwst();
		return false;
	}

	bool visit(Conditional const& _n) override
	{
		m_result = SolConditional(m_ctx, _n).toAwst();
		return false;
	}

	bool visit(Assignment const& _n) override
	{
		m_result = SolAssignment(m_ctx, _n).toAwst();
		return false;
	}

	bool visit(IndexAccess const& _n) override
	{
		m_result = SolIndexAccess(m_ctx, _n).toAwst();
		return false;
	}

	bool visit(IndexRangeAccess const& _n) override
	{
		m_result = SolIndexRangeAccess(m_ctx, _n).toAwst();
		return false;
	}

	bool visit(TupleExpression const& _n) override
	{
		m_result = SolTupleExpression(m_ctx, _n).toAwst();
		return false;
	}

	bool visit(FunctionCall const& _n) override
	{
		SolExpressionFactory factory(m_ctx);
		auto handler = factory.createFunctionCall(_n);
		if (handler)
			m_result = handler->toAwst();
		else
		{
			Logger::instance().error("unhandled function call kind", makeLoc(_n));
			m_result = makeVoid(_n);
		}
		return false;
	}

	bool visit(MemberAccess const& _n) override
	{
		m_result = buildMemberAccess(_n);
		return false;
	}

	bool visit(FunctionCallOptions const& _n) override
	{
		// Call handlers consume options directly; a standalone function value
		// must still evaluate its options even without invoking the function.
		m_result = SolMemberAccess::projectFunctionValue(m_ctx, _n,
			m_ctx.typeMapper.map(_n.annotation().type), makeLoc(_n),
			[&](Expression const& source) { return buildExpression(m_ctx, source); });
		return false;
	}

	bool visit(ElementaryTypeNameExpression const& _n) override
	{
		m_result = makeVoid(_n);
		return false;
	}

private:
	eb::ContractContext& m_ctx;
	std::shared_ptr<awst::Expression> m_result;

	bool visitNode(ASTNode const& _node) override
	{
		Logger::instance().error("unhandled expression type", makeLoc(_node));
		m_result = makeVoid(_node);
		return false;
	}

	std::shared_ptr<awst::Expression> buildMemberAccess(MemberAccess const& _n)
	{
		// A proven pointer-cast body can disappear, but its normally bound
		// operands must execute. Keep the selected location, not its contents.
		if (!m_ctx.typeMapper.profile().evmStorageLayout)
		if (auto const* call = SolcFacts::expressionAs<FunctionCall>(&_n.expression()))
		if (auto const* function = SolcFacts::resolveInternalCall(*call, m_ctx.currentContract))
		if (auto const& alias = m_ctx.typeMapper.analysis().storageReturnFacts(function).pointerAlias;
			alias && alias->field == _n.memberName())
		{
			auto const& source = *SolcFacts::callArguments(*call).at(alias->parameter);
			if (_n.annotation().willBeWrittenTo)
				for (auto const* root: SolcFacts::referenceSources(source))
					if (auto const* id = SolcFacts::expressionAs<Identifier>(root))
					if (auto const* parameter = dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration);
						parameter && parameter->referenceLocation() == VariableDeclaration::Location::Storage)
					if (auto const* owner = dynamic_cast<FunctionDefinition const*>(parameter->scope()))
					{
						auto const& plan = m_ctx.typeMapper.callBoundaryPlan(*owner, m_ctx.currentContract);
						for (size_t i = 0; i < plan.parameters.size(); ++i)
							if (plan.parameters[i].declaration == parameter
								&& plan.parameters[i].passing == RefParamPassing::Value
								&& std::find(plan.writeBackParams.begin(), plan.writeBackParams.end(), i)
									== plan.writeBackParams.end())
								Logger::instance().error(
									"write through this value-carried storage parameter has no write-back; "
									"use --evm-storage-layout for a write-through slot reference", makeLoc(_n));
					}
			std::shared_ptr<awst::Expression> target;
			CallOperands::buildParameters(m_ctx, *call, makeLoc(_n),
				[&](Expression const& argument, size_t index) -> std::shared_ptr<awst::Expression> {
					if (index != alias->parameter) return buildExpression(m_ctx, argument);
					target = ResolvedLValue::freezeTarget(m_ctx, buildExpression(m_ctx, argument), makeLoc(argument));
					return awst::makeVoidConstant(makeLoc(argument));
				});
			return target;
		}

		// Scalar-leaf read on a >4KB blob aggregate (`p.w1.x`): route through
		// the multi-slot blob. resolveBlobOffset no-ops for non-blob-agg roots.
		if (m_ctx.currentScope)
		{
			auto const* nt = _n.annotation().type;
			if (nt)
			{
				auto loc0 = makeLoc(_n);
				if (auto off = SolIndexAccess::resolveBlobOffset(m_ctx, *m_ctx.currentScope, _n, loc0))
					if (auto val = SolIndexAccess::readBlobValue(m_ctx, std::move(off), nt, loc0))
						return val;
			}
		}

		SolExpressionFactory factory(m_ctx);
		auto handler = factory.createMemberAccess(_n);
		if (handler)
		{
			auto result = handler->toAwst();
			if (result) return result;
		}

		// Fallback: sol-eb builder dispatch on the base value's instance builder.
		auto base = buildExpression(m_ctx, _n.expression());
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

		// LVALUE position (solc's willBeWrittenTo): a placeholder here becomes
		// an assignment target and the write silently goes nowhere — fail loud
		// at the source instead (the assignment factories carry a second net).
		if (_n.annotation().willBeWrittenTo)
		{
			Logger::instance().error(
				"unsupported member access '." + _n.memberName()
				+ "' in assignment-target position — the write cannot be "
				"resolved to a storage or memory location", loc);
			return awst::makeBytesConstant({}, loc);
		}
		// A qualified type name (L.Struct, L.Enum) has no runtime value.
		if (dynamic_cast<TypeType const*>(_n.annotation().type))
			return awst::makeVoidConstant(loc);
		// ABI builtins are callable only by name; a bare `abi.encode;` is inert.
		if (auto const* magic = dynamic_cast<MagicType const*>(baseSolType);
			magic && magic->kind() == MagicType::Kind::ABI)
			return awst::makeVoidConstant(loc);
		// Declaration (C.f in abi.encodeCall), unbound library methods and
		// bare UDVT wrap/unwrap names are metadata, not runtime function values.
		if (auto const* function = dynamic_cast<FunctionType const*>(_n.annotation().type);
			function && (function->kind() == FunctionType::Kind::Declaration
				|| (function->kind() == FunctionType::Kind::DelegateCall
					&& dynamic_cast<TypeType const*>(baseSolType))
				|| function->kind() == FunctionType::Kind::Wrap
				|| function->kind() == FunctionType::Kind::Unwrap))
			return awst::makeVoidConstant(loc);
		Logger::instance().error(
			"unsupported runtime member access '." + _n.memberName() + "'", loc);
		return awst::makeVoidConstant(loc);
	}

	awst::SourceLocation makeLoc(solidity::frontend::ASTNode const& _node)
	{
		return m_ctx.makeLoc(_node.location());
	}

	std::shared_ptr<awst::Expression> makeVoid(solidity::frontend::ASTNode const& _node)
	{
		auto vc = awst::makeVoidConstant(makeLoc(_node));
		return vc;
	}
};

} // anonymous namespace

std::shared_ptr<awst::Expression> buildExpression(
	eb::ContractContext& _ctx,
	Expression const& _expr)
{
	return SolExpressionVisitor(_ctx).build(_expr);
}

} // namespace puyasol::builder::sol_ast
