#pragma once

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <set>

namespace puyasol::builder
{

/// Walks a function body to find which parameters are assigned to.
///
/// A param is "mutated" if it (or a sub-component via index/member/tuple)
/// appears on the LHS of an Assignment. Used in memory-ref/storage-ref
/// augmentation: callees that don't mutate a ref param skip the caller-side
/// write-back, saving a tuple slot. Shared by AWSTBuilder.cpp and
/// SolInternalCall.cpp to keep semantics identical.
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
