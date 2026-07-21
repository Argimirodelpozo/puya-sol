#pragma once

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

/// Conservative static scans for OperandPlan sequencing. The pending-delta
/// capture only sees QUEUED effects (write-backs, inc/dec spills); calls that
/// write state directly (handle-model storage params, plain state writes in
/// the callee) execute inline at expression-eval time and are invisible to
/// it. These scans over the solc AST decide when an operand must be pinned to
/// a pre-statement so effects land in legacy-solc evaluation order.
class EffectScan: public solidity::frontend::ASTConstVisitor
{
public:
	bool found = false;

	/// Does evaluating `e` possibly WRITE state (storage, memory refs, a
	/// deploy, a local via ++/--/assign)? Over-approximates: an extra pin is
	/// harmless, a missed one reorders a visible effect.
	static bool mayWrite(solidity::frontend::Expression const& e)
	{
		EffectScan s;
		e.accept(s);
		return s.found;
	}

	bool visit(solidity::frontend::FunctionCall const& c) override
	{
		using solidity::frontend::FunctionCallKind;
		auto kind = *c.annotation().kind;
		if (kind == FunctionCallKind::TypeConversion
			|| kind == FunctionCallKind::StructConstructorCall)
			return !found; // no write itself; keep scanning the argument
		auto const* ft = dynamic_cast<solidity::frontend::FunctionType const*>(
			c.expression().annotation().type);
		using SM = solidity::frontend::StateMutability;
		if (ft && (ft->stateMutability() == SM::Pure || ft->stateMutability() == SM::View))
			return !found; // reads only; args may still write — keep scanning
		found = true;
		return false;
	}

	bool visit(solidity::frontend::Assignment const&) override
	{
		found = true;
		return false;
	}

	bool visit(solidity::frontend::UnaryOperation const& u) override
	{
		using solidity::langutil::Token;
		auto op = u.getOperator();
		if (op == Token::Inc || op == Token::Dec || op == Token::Delete)
		{
			found = true;
			return false;
		}
		return !found;
	}

	bool visit(solidity::frontend::NewExpression const&) override
	{
		found = true;
		return false;
	}
};

/// Is `e` built ONLY from literals, local/constant variables, and pure
/// operators over them — i.e. a value no sibling's state write can disturb?
/// Whitelist walk: anything unrecognized (state reads, member/index access,
/// calls) returns false, which just costs a redundant pin.
inline bool onlyLocalPure(solidity::frontend::Expression const& e)
{
	using namespace solidity::frontend;
	if (dynamic_cast<Literal const*>(&e))
		return true;
	if (auto const* id = dynamic_cast<Identifier const*>(&e))
	{
		auto const* vd = dynamic_cast<VariableDeclaration const*>(
			id->annotation().referencedDeclaration);
		if (!vd || !(vd->isLocalOrReturn() || vd->isConstant()))
			return false;
		// A local STORAGE pointer reads storage on use — not a plain local.
		return !(vd->type() && vd->type()->dataStoredIn(DataLocation::Storage));
	}
	if (auto const* tup = dynamic_cast<TupleExpression const*>(&e))
	{
		for (auto const& c: tup->components())
			if (c && !onlyLocalPure(*c))
				return false;
		return !tup->isInlineArray();
	}
	if (auto const* u = dynamic_cast<UnaryOperation const*>(&e))
	{
		using solidity::langutil::Token;
		auto op = u->getOperator();
		if (op == Token::Inc || op == Token::Dec || op == Token::Delete)
			return false;
		return onlyLocalPure(u->subExpression());
	}
	if (auto const* b = dynamic_cast<BinaryOperation const*>(&e))
		return onlyLocalPure(b->leftExpression()) && onlyLocalPure(b->rightExpression());
	if (auto const* cond = dynamic_cast<Conditional const*>(&e))
		return onlyLocalPure(cond->condition())
			&& onlyLocalPure(cond->trueExpression())
			&& onlyLocalPure(cond->falseExpression());
	return false;
}

} // namespace puyasol::builder
