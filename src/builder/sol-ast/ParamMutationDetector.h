#pragma once

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <set>

namespace puyasol::builder
{

/// Walks a function body to find which of its parameters are written to.
///
/// A parameter is considered written to if it (or any sub-component
/// reached via index / member / tuple destructure) appears on the LHS of
/// an Assignment. Use-def analysis for memory-ref / storage-ref param
/// augmentation: callees that DON'T mutate a ref param need no caller-
/// side write-back, which saves a tuple slot per uneeded param.
///
/// Two call sites use this — AWSTBuilder.cpp (during contract translation)
/// and SolInternalCall.cpp (during internal-call lowering). Both need
/// identical semantics; sharing the implementation here avoids drift.
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
