#pragma once

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <set>

namespace puyasol::builder
{

/// Walks a function body to find which parameters are written to.
///
/// A param is "mutated" if it (or a sub-component via index/member/tuple)
/// is the target of an Assignment, of `++`/`--`/`delete`, or the receiver of
/// a mutating array member call (`p.arr.push(x)`, `p.arr.pop()`). Used in
/// memory-ref/storage-ref augmentation: callees that don't mutate a ref param
/// skip the caller-side write-back, saving a tuple slot; also gates the
/// aliased-arg Copy break (Copy is only safe for unmutated params). Shared by
/// AWSTBuilder.cpp, FunctionBuilder.cpp and SolInternalCall.cpp to keep
/// semantics identical. Known gap: mutation via passing the param on to
/// ANOTHER mutating callee is not tracked (needs call-graph closure).
///
/// Usage:
///     ParamMutationDetector det;
///     for (auto const& p: func->parameters()) det.paramIds.insert(p->id());
///     func->body().accept(det);
///     bool isMutated = det.mutated.count(paramId);
class ParamMutationDetector: public solidity::frontend::ASTConstVisitor
{
public:
	std::set<int64_t> paramIds;  // input: param decl ids to track
	std::set<int64_t> mutated;   // output: subset of paramIds written to

	bool visit(solidity::frontend::Assignment const& a) override
	{
		recordRoot(&a.leftHandSide());
		return true;  // recurse so nested assignments on the RHS also count
	}

	bool visit(solidity::frontend::UnaryOperation const& u) override
	{
		using solidity::langutil::Token;
		auto op = u.getOperator();
		if (op == Token::Inc || op == Token::Dec || op == Token::Delete)
			recordRoot(&u.subExpression());
		return true;
	}

	bool visit(solidity::frontend::FunctionCall const& c) override
	{
		// `p.push(x)` / `p.pop()` mutate the receiver. Match on the member
		// name only — over-recording is safe (an extra write-back is
		// redundant, a missed one loses the mutation).
		if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&c.expression()))
			if (ma->memberName() == "push" || ma->memberName() == "pop")
				recordRoot(&ma->expression());
		return true;
	}

private:
	void recordRoot(solidity::frontend::Expression const* lhs)
	{
		using namespace solidity::frontend;
		while (true)
		{
			if (auto const* ix = dynamic_cast<IndexAccess const*>(lhs))
				{ lhs = &ix->baseExpression(); continue; }
			if (auto const* mb = dynamic_cast<MemberAccess const*>(lhs))
				{ lhs = &mb->expression(); continue; }
			if (auto const* tup = dynamic_cast<TupleExpression const*>(lhs))
			{
				for (auto const& c: tup->components())
					if (c) recordRoot(c.get());
				return;
			}
			break;
		}
		if (auto const* id = dynamic_cast<Identifier const*>(lhs))
		{
			auto const* decl = id->annotation().referencedDeclaration;
			if (decl && paramIds.count(decl->id()))
				mutated.insert(decl->id());
		}
	}
};

} // namespace puyasol::builder
