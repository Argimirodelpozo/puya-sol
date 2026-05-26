#include "builder/contract/PostInitTriggers.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <functional>
#include <set>

namespace puyasol::builder
{

namespace {

/// Checks if a Solidity AST subtree references any state variable whose AST ID
/// is in the given set (i.e. box-stored state variables).
class BoxVarRefChecker: public solidity::frontend::ASTConstVisitor
{
public:
	explicit BoxVarRefChecker(std::set<int64_t> const& _boxVarIds): m_boxVarIds(_boxVarIds) {}
	bool found() const { return m_found; }

	bool visit(solidity::frontend::Identifier const& _node) override
	{
		if (m_found)
			return false;
		if (auto const* decl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				_node.annotation().referencedDeclaration))
		{
			if (m_boxVarIds.count(decl->id()))
				m_found = true;
		}
		return !m_found;
	}

private:
	std::set<int64_t> const& m_boxVarIds;
	bool m_found = false;
};

/// Walks every constructor body in `_contract`'s linearised inheritance
/// chain (current contract first, then bases), running `_checker` against
/// each body. Stops as soon as `_done` returns true.
template <class V>
void walkAllConstructors(
	solidity::frontend::ContractDefinition const& _contract,
	V& _checker,
	std::function<bool()> const& _done)
{
	if (auto const* ctor = _contract.constructor())
		if (ctor->isImplemented() && !ctor->body().statements().empty())
		{
			ctor->body().accept(_checker);
			if (_done()) return;
		}

	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base == &_contract)
			continue;
		auto const* baseCtor = base->constructor();
		if (baseCtor && baseCtor->isImplemented()
			&& !baseCtor->body().statements().empty())
		{
			baseCtor->body().accept(_checker);
			if (_done()) return;
		}
	}
}

/// Walks every state-variable initializer expression in the contract's
/// linearised chain.
template <class V>
void walkAllStateVarInits(
	solidity::frontend::ContractDefinition const& _contract,
	V& _checker)
{
	forEachStateVar(_contract, [&](auto const* var)
	{
		if (var->value())
			var->value()->accept(_checker);
	});
}

/// Box-write detector — true if the constructor (directly or transitively
/// via a function call) touches any box-stored state variable.
bool detectBoxRefsInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	std::set<int64_t> boxVarIds;
	forEachStateVar(_contract, [&](auto const* var)
	{
		if (var->isConstant())
			return;
		if (StorageMapper::shouldUseBoxStorage(*var))
			boxVarIds.insert(var->id());
	});
	if (boxVarIds.empty())
		return false;

	BoxVarRefChecker direct(boxVarIds);
	walkAllConstructors(_contract, direct, [&]{ return direct.found(); });
	if (direct.found())
		return true;

	// Indirect: collect AST IDs of non-constructor functions that touch
	// box-stored vars, then re-walk constructors looking for calls to them.
	std::set<int64_t> boxWriteFuncIds;
	forEachDefinedFunction(_contract, [&](auto const* func)
	{
		if (func->isConstructor() || !func->isImplemented())
			return;
		BoxVarRefChecker fnCheck(boxVarIds);
		func->body().accept(fnCheck);
		if (fnCheck.found())
			boxWriteFuncIds.insert(func->id());
	});
	if (boxWriteFuncIds.empty())
		return false;

	struct CtorCallChecker: public solidity::frontend::ASTConstVisitor
	{
		std::set<int64_t> const& targetIds;
		bool found = false;
		explicit CtorCallChecker(std::set<int64_t> const& _ids): targetIds(_ids) {}
		bool visit(solidity::frontend::FunctionCall const& _node) override
		{
			if (found) return false;
			// solc's ASTNode::referencedDeclaration covers both
			// `f(...)` (Identifier) and `Lib.f(...)` (MemberAccess) —
			// the original Identifier-only check missed library /
			// base-qualified calls into known box-touching functions.
			auto const* decl = solidity::frontend::ASTNode::referencedDeclaration(_node.expression());
			if (decl && targetIds.count(decl->id()))
				found = true;
			return !found;
		}
	};
	CtorCallChecker callChecker(boxWriteFuncIds);
	walkAllConstructors(_contract, callChecker, [&]{ return callChecker.found; });
	return callChecker.found;
}

/// `new C(...)` detector — child contracts deployed via inner-create need
/// the parent to already hold balance, which only lands after AppCreate.
bool detectNewExprInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	struct NewExprChecker: public solidity::frontend::ASTConstVisitor
	{
		bool found = false;
		bool visit(solidity::frontend::NewExpression const&) override
		{ found = true; return false; }
	};
	NewExprChecker checker;
	walkAllStateVarInits(_contract, checker);
	if (auto const* ctor = _contract.constructor())
		if (ctor->isImplemented())
			ctor->body().accept(checker);
	return checker.found;
}

/// `msg.value` / `msg.sender` / `msg.data` detector — these only resolve
/// correctly inside `__postInit` when the parent groups the Payment with
/// the post-init call (not with AppCreate).
bool detectMsgRefInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	struct MsgRefChecker: public solidity::frontend::ASTConstVisitor
	{
		bool found = false;
		bool visit(solidity::frontend::MemberAccess const& _ma) override
		{
			// Resolve via solc's MagicVariableDeclaration so a user-defined
			// local named `msg` doesn't trigger a false positive — and a
			// member-access path like `(somelookup).value` doesn't accidentally
			// look like `msg.value`. The magic globals msg/block/tx are tagged
			// as MagicVariableDeclaration on the resolved Identifier.
			auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&_ma.expression());
			if (!id) return !found;
			auto const* magic = dynamic_cast<solidity::frontend::MagicVariableDeclaration const*>(
				id->annotation().referencedDeclaration);
			if (magic && magic->name() == "msg"
				&& (_ma.memberName() == "value"
					|| _ma.memberName() == "sender"
					|| _ma.memberName() == "data"))
				found = true;
			return !found;
		}
	};
	MsgRefChecker checker;
	walkAllStateVarInits(_contract, checker);
	if (auto const* ctor = _contract.constructor())
		if (ctor->isImplemented())
			ctor->body().accept(checker);
	return checker.found;
}

/// `AVM.<x>(...)` detector — the stdlib library issues inner txns that
/// require the contract to already hold MBR + ASA reserves, which can
/// only land via a pay txn AFTER AppCreate. Defers to __postInit.
bool detectAvmLibCallInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	struct AvmLibCallChecker: public solidity::frontend::ASTConstVisitor
	{
		bool found = false;
		bool visit(solidity::frontend::MemberAccess const& _ma) override
		{
			if (found) return false;
			// solc's ASTNode::referencedDeclaration handles both
			// `AVM.foo()` (Identifier base) and module-aliased forms
			// `import "tokens/AVM.sol" as Mod; Mod.AVM.foo()`
			// (MemberAccess base) without a per-shape dynamic_cast.
			if (auto const* contractDef = dynamic_cast<solidity::frontend::ContractDefinition const*>(
					solidity::frontend::ASTNode::referencedDeclaration(_ma.expression())))
				if (contractDef->isLibrary() && contractDef->name() == "AVM")
					found = true;
			return !found;
		}
	};
	AvmLibCallChecker checker;
	walkAllStateVarInits(_contract, checker);
	walkAllConstructors(_contract, checker, [&]{ return checker.found; });
	return checker.found;
}

} // namespace

/// Combines the four post-init triggers; logs which one fired (via debug).
bool computeNeedsPostInit(solidity::frontend::ContractDefinition const& _contract)
{
	if (detectBoxRefsInConstructor(_contract))
		return true;
	if (detectNewExprInConstructor(_contract))
	{
		Logger::instance().debug("Forcing __postInit: constructor/state-init deploys child contracts via new C()");
		return true;
	}
	if (detectMsgRefInConstructor(_contract))
	{
		Logger::instance().debug("Forcing __postInit: constructor/state-init references msg.*");
		return true;
	}
	if (detectAvmLibCallInConstructor(_contract))
	{
		Logger::instance().debug("Forcing __postInit: constructor/state-init calls into AVM stdlib");
		return true;
	}
	return false;
}

} // namespace puyasol::builder
