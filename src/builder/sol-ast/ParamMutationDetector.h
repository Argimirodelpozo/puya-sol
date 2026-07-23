#pragma once

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <set>

namespace puyasol::builder
{

/// Walks a function body to find which parameters are written to.
///
/// A param is "mutated" if it (or a sub-component via index/member/tuple)
/// is the target of an Assignment, of `++`/`--`/`delete`, the receiver of
/// a mutating array member call (`p.arr.push(x)`, `p.arr.pop()`), or is
/// PASSED ON to another internal callee whose corresponding REFERENCE param
/// is (transitively) mutated — the call-graph closure (possible_solc item 3;
/// was a documented silent write-back drop: `outer(S storage p){inner(p);}`
/// skipped the caller-side write-back). Used in memory-ref/storage-ref
/// augmentation: callees that don't mutate a ref param skip the caller-side
/// write-back, saving a tuple slot; also gates the aliased-arg Copy break
/// (Copy is only safe for unmutated params). Shared by AWSTBuilder.cpp,
/// FunctionBuilder.cpp and SolInternalCall.cpp — extending THIS class keeps
/// all three in lockstep. Residual: virtual dispatch resolves to the DECLARED
/// target (an override mutating more than its base is missed); recursion
/// cycles over-approximate (all ref params mutated — extra write-backs are
/// redundant-but-correct, and an over-declined aliasing Copy fails loud).
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
		using namespace solidity::frontend;
		// `p.push(x)` / `p.pop()` mutate the receiver. Match on the member
		// name only — over-recording is safe (an extra write-back is
		// redundant, a missed one loses the mutation).
		if (auto const* ma = dynamic_cast<MemberAccess const*>(&c.expression()))
			if (ma->memberName() == "push" || ma->memberName() == "pop")
				recordRoot(&ma->expression());

		// Call-graph closure: an arg passed into a callee REFERENCE param the
		// callee (transitively) mutates counts as a mutation of the arg's root.
		auto const* fd = resolveCalleeFunction(c);
		if (fd && fd->isImplemented())
		{
			auto const& calleeMut = transitivelyMutated(*fd);
			if (!calleeMut.empty())
			{
				auto const& params = fd->parameters();
				bool bound = false;
				if (auto const* ft = dynamic_cast<FunctionType const*>(
						c.expression().annotation().type))
					bound = ft->hasBoundFirstArgument();
				// using-for receiver = param 0
				if (bound && !params.empty())
					if (auto const* recv = dynamic_cast<MemberAccess const*>(&c.expression());
						recv && refParamMutated(*params[0], calleeMut))
						recordRoot(&recv->expression());
				auto args = c.sortedArguments();
				size_t shift = bound ? 1 : 0;
				for (size_t i = 0; i < args.size(); ++i)
				{
					size_t pi = i + shift;
					if (pi >= params.size() || !args[i])
						continue;
					if (refParamMutated(*params[pi], calleeMut))
						recordRoot(args[i].get());
				}
			}
		}
		return true;
	}

	/// Transitively-mutated param decl ids of `_f` (direct + via callees).
	/// Process-wide memo (single compile per process, same precedent as
	/// boxKeyedStructRegistry); recursion cycles over-approximate to ALL
	/// reference params of the in-progress function.
	static std::set<int64_t> const& transitivelyMutated(
		solidity::frontend::FunctionDefinition const& _f)
	{
		static std::map<int64_t, std::set<int64_t>> s_cache;
		static std::set<int64_t> s_inProgress;
		if (auto it = s_cache.find(_f.id()); it != s_cache.end())
			return it->second;
		if (s_inProgress.count(_f.id()))
		{
			// Cycle: conservatively treat every reference param as mutated.
			std::set<int64_t> all;
			for (auto const& p: _f.parameters())
				if (isReferenceParam(*p))
					all.insert(p->id());
			return s_cache.emplace(_f.id(), std::move(all)).first->second;
		}
		s_inProgress.insert(_f.id());
		ParamMutationDetector det;
		for (auto const& p: _f.parameters())
			det.paramIds.insert(p->id());
		if (_f.isImplemented())
			_f.body().accept(det);
		s_inProgress.erase(_f.id());
		// A cycle hit may have cached the conservative set meanwhile — the
		// computed (tighter) result wins.
		return s_cache.insert_or_assign(_f.id(), std::move(det.mutated)).first->second;
	}

private:
	/// The internal-call target, when statically resolvable. Virtual dispatch
	/// resolves to the DECLARED target (documented residual).
	static solidity::frontend::FunctionDefinition const* resolveCalleeFunction(
		solidity::frontend::FunctionCall const& _c)
	{
		using namespace solidity::frontend;
		if (*_c.annotation().kind != FunctionCallKind::FunctionCall)
			return nullptr;
		Declaration const* decl = nullptr;
		if (auto const* id = dynamic_cast<Identifier const*>(&_c.expression()))
			decl = id->annotation().referencedDeclaration;
		else if (auto const* ma = dynamic_cast<MemberAccess const*>(&_c.expression()))
			decl = ma->annotation().referencedDeclaration;
		return dynamic_cast<FunctionDefinition const*>(decl);
	}

	/// Only REFERENCE params write through to the caller (storage refs and
	/// memory aggregates — our model write-backs both). Value params are
	/// callee-local copies.
	static bool isReferenceParam(solidity::frontend::VariableDeclaration const& _p)
	{
		using namespace solidity::frontend;
		if (_p.referenceLocation() == VariableDeclaration::Location::Storage)
			return true;
		return _p.referenceLocation() == VariableDeclaration::Location::Memory
			&& _p.type() && !_p.type()->isValueType();
	}

	static bool refParamMutated(
		solidity::frontend::VariableDeclaration const& _p,
		std::set<int64_t> const& _mut)
	{
		return isReferenceParam(_p) && _mut.count(_p.id()) != 0;
	}

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
