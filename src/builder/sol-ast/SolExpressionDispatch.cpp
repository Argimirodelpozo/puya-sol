/// @file SolExpressionDispatch.cpp
/// Central expression dispatcher using solc's ASTConstVisitor. Handlers own
/// child lowering and evaluation order; returning false disables the child walk.

#include "builder/sol-ast/AsmScan.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/sol-ast/SolExpressionFactory.h"
#include "builder/sol-ast/exprs/SolLiteral.h"
#include "builder/sol-ast/exprs/SolConditional.h"
#include "builder/sol-ast/exprs/SolIdentifier.h"
#include "builder/sol-ast/exprs/SolTupleExpression.h"
#include "builder/sol-ast/exprs/SolBinaryOperation.h"
#include "builder/sol-ast/exprs/SolUnaryOperation.h"
#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

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
		_n.accept(*this);
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
		// Call handlers consume options; a non-call wrapper just forwards its base.
		m_result = buildExpression(m_ctx, _n.expression());
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
		// STORAGE-POINTER ALIAS: `StorageSlot.getStringSlot(store).value` denotes
		// the same storage location as `store`, so resolve to the argument and
		// skip the call entirely. Solidity forbids assigning to a storage
		// pointer, which is exactly why OZ routes writes through this wrapper —
		// so this must produce an LVALUE, not a copy. See
		// AsmScan.h::storagePointerAliasParam for the exact shape required.
		// --evm-storage-layout: NOT needed — the call returns a real biguint
		// slot handle and member access/writes resolve through it (and
		// contract-method storage params work: slots write straight through).
		if (!m_ctx.typeMapper.profile().evmStorageLayout)
		if (auto const* call = dynamic_cast<FunctionCall const*>(&_n.expression()))
		{
			Declaration const* refDecl = nullptr;
			if (auto const* ma = dynamic_cast<MemberAccess const*>(&call->expression()))
				refDecl = ma->annotation().referencedDeclaration;
			else if (auto const* id = dynamic_cast<Identifier const*>(&call->expression()))
				refDecl = id->annotation().referencedDeclaration;
			if (auto const* fd = dynamic_cast<FunctionDefinition const*>(refDecl))
				if (auto alias = builder::storagePointerAliasParam(*fd))
					if (alias->second == _n.memberName()
						&& alias->first < call->arguments().size())
					{
						auto const* arg = call->arguments()[alias->first].get();
						// Writing through a bytes/string storage-ref PARAM only
						// reaches the caller's state when the enclosing function
						// is a LIBRARY/free function — those get the storage
						// write-back augmentation (buildFreestandingSubroutine);
						// contract methods do not, so the store would vanish.
						// That combination was previously unreachable (this alias
						// is the only legal way to write through such a param),
						// and it must not become a SILENT dropped write.
						if (auto const* aid = dynamic_cast<Identifier const*>(arg))
							if (auto const* pv = dynamic_cast<VariableDeclaration const*>(
									aid->annotation().referencedDeclaration))
								if (pv->isCallableOrCatchParameter()
									&& pv->referenceLocation()
										== VariableDeclaration::Location::Storage)
								{
									auto const* owner = dynamic_cast<FunctionDefinition const*>(
										pv->scope());
									auto const* c = owner ? owner->annotation().contract : nullptr;
									if (!c || !c->isLibrary())
										Logger::instance().error(
											"write through a storage-ref parameter of a "
											"contract method is not supported — only "
											"library/free functions get storage write-back, "
											"so this store would be dropped. Move the helper "
											"into a library.",
											makeLoc(_n));
								}
						return buildExpression(m_ctx, *arg);
					}
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
		// Warning (not error): TypeType member access like `MyType.wrap;` (no
		// invocation) emits a typed zero — value never used at runtime.
		Logger::instance().warning(
			"unsupported member access '." + _n.memberName() + "'", loc);
		auto* wtype = m_ctx.typeMapper.map(_n.annotation().type);
		if (awst::isNumericWType(wtype))
			return awst::makeZero(loc, wtype);
		if (wtype == awst::WType::boolType())
			return awst::makeBoolConstant(false, loc, wtype);
		return awst::makeBytesConstant({}, loc);
	}

	awst::SourceLocation makeLoc(solidity::frontend::ASTNode const& _node)
	{
		auto const& l = _node.location();
		return m_ctx.makeLoc(l.start, l.end);
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
