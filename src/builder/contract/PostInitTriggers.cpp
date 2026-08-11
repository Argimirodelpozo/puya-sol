#include "builder/contract/PostInitTriggers.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <functional>
#include <set>

namespace puyasol::builder
{

namespace {

/// Check whether an AST subtree references any box-stored state variable (by AST ID).
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

/// Walk all constructor bodies in MRO order; stop when _done() returns true.
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

/// Walk every state-variable initializer in the linearised chain.
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

/// True if the constructor (directly or via a function call) touches box-stored state.
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

	// Indirect: collect IDs of non-ctor functions that touch box vars,
	// then re-walk constructors for calls to any of them.
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
			// ASTNode::referencedDeclaration covers Identifier and MemberAccess
			// (catches Lib.f() that an Identifier-only check would miss).
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

/// True if the constructor deploys child contracts via new C() (requires balance → post AppCreate).
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

/// True if the constructor (incl. inherited ctors / state-var inits) performs
/// an EXTERNAL call — an inner txn on AVM, whose foreign-app/asset refs the
/// bare create txn cannot carry (resource population only reaches the
/// __postInit call). The Aave WrappedTokenGatewayV3 ctor calls its pool dep
/// at construction: inline in the create txn that dies with "unavailable
/// App". `this.f()` self-calls lower to subroutines — skipped.
bool detectExternalCallInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	struct ExternalCallChecker: public solidity::frontend::ASTConstVisitor
	{
		bool found = false;
		bool visit(solidity::frontend::FunctionCall const& _fc) override
		{
			if (found) return false;
			auto const* ft = dynamic_cast<solidity::frontend::FunctionType const*>(
				_fc.expression().annotation().type);
			if (!ft) return true;
			using K = solidity::frontend::FunctionType::Kind;
			switch (ft->kind())
			{
			case K::External:
			case K::BareCall:
			case K::BareStaticCall:
			case K::Send:
			case K::Transfer:
				break;
			default:
				return true; // internal/library/event/... — no inner txn
			}
			// `this.f()` is a subroutine call on AVM, not an inner txn.
			if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(
					&_fc.expression()))
				if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(
						&ma->expression()))
					if (id->name() == "this"
						&& dynamic_cast<solidity::frontend::MagicVariableDeclaration const*>(
							id->annotation().referencedDeclaration))
						return true;
			found = true;
			return false;
		}
	};
	ExternalCallChecker checker;
	walkAllStateVarInits(_contract, checker);
	walkAllConstructors(_contract, checker, [&]{ return checker.found; });
	return checker.found;
}

/// True if the constructor reads msg.value/sender/data (only valid in __postInit group).
bool detectMsgRefInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	struct MsgRefChecker: public solidity::frontend::ASTConstVisitor
	{
		bool found = false;
		bool visit(solidity::frontend::MemberAccess const& _ma) override
		{
			// Use MagicVariableDeclaration to avoid false-positives from user-defined
			// `msg` locals or `(somelookup).value` member-access paths.
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

/// True if the constructor's call closure contains inline assembly. Asm
/// sload/sstore lowers to the storage dispatcher, whose box-per-slot fallback
/// creates boxes of the app itself — impossible during the create txn (the app
/// id doesn't exist yet), so such ctor bodies must run in __postInit.
/// Conservative: asm-for-pure-math ctors get postInit too (harmless).
bool detectAsmInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	struct AsmDetector: public solidity::frontend::ASTConstVisitor
	{
		bool found = false;
		bool visit(solidity::frontend::InlineAssembly const&) override
		{ found = true; return false; }
	};
	AsmDetector direct;
	walkAllConstructors(_contract, direct, [&]{ return direct.found; });
	if (direct.found)
		return true;

	// Indirect (one level): ctor calls a function whose body has asm.
	std::set<int64_t> asmFuncIds;
	forEachDefinedFunction(_contract, [&](auto const* func)
	{
		if (func->isConstructor() || !func->isImplemented())
			return;
		AsmDetector fnCheck;
		func->body().accept(fnCheck);
		if (fnCheck.found)
			asmFuncIds.insert(func->id());
	});
	if (asmFuncIds.empty())
		return false;

	struct CallChecker: public solidity::frontend::ASTConstVisitor
	{
		std::set<int64_t> const& targetIds;
		bool found = false;
		explicit CallChecker(std::set<int64_t> const& t): targetIds(t) {}
		bool visit(solidity::frontend::Identifier const& _node) override
		{
			if (found) return false;
			if (auto const* fn = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
					_node.annotation().referencedDeclaration))
				if (targetIds.count(fn->id()))
					found = true;
			return !found;
		}
	};
	CallChecker calls(asmFuncIds);
	walkAllConstructors(_contract, calls, [&]{ return calls.found; });
	return calls.found;
}

/// True if the constructor calls AVM stdlib (inner txns need MBR+ASA → post AppCreate).
bool detectAvmLibCallInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	struct AvmLibCallChecker: public solidity::frontend::ASTConstVisitor
	{
		bool found = false;
		bool visit(solidity::frontend::MemberAccess const& _ma) override
		{
			if (found) return false;
			// referencedDeclaration handles both `AVM.foo()` and aliased `Mod.AVM.foo()`.
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

bool computeNeedsPostInit(solidity::frontend::ContractDefinition const& _contract)
{
	// --evm-storage-layout: EVERY state write is a box write, and boxes of the
	// app being created cannot be referenced in the create txn — defer any
	// state initializer / constructor body to __postInit.
	if (evmStorageLayout())
	{
		// INHERITED constructors count too. A contract with no ctor of its own
		// but an `Ownable` base still runs `_transferOwnership` during create,
		// and OZ reads `_owner` before writing it — a box READ in this mode.
		// A box cannot be referenced in the create txn at all: the app account
		// does not exist yet to hold the box's minimum balance, so even a
		// resource-populated create fails ("balance 0 below min"). Checking
		// only the contract's OWN constructor left friend.tech without a
		// __postInit, so it deployed in the default model (owner is app-global
		// there) and died in slot mode on `invalid Box reference "p:"++itob(0)`.
		bool anyStateWork = _contract.constructor() != nullptr;
		if (!anyStateWork)
			for (auto const* base: _contract.annotation().linearizedBaseContracts)
				if (base && base->constructor())
				{
					anyStateWork = true;
					break;
				}
		if (!anyStateWork)
			for (auto const* base: _contract.annotation().linearizedBaseContracts)
				for (auto const* var: base->stateVariables())
					if (var && var->value() && !var->isConstant() && !var->immutable())
						anyStateWork = true;
		if (anyStateWork)
		{
			Logger::instance().debug(
				"Forcing __postInit: --evm-storage-layout state writes are box ops");
			return true;
		}
	}
	if (detectBoxRefsInConstructor(_contract))
		return true;
	if (detectNewExprInConstructor(_contract))
	{
		Logger::instance().debug("Forcing __postInit: constructor/state-init deploys child contracts via new C()");
		return true;
	}
	if (detectExternalCallInConstructor(_contract))
	{
		Logger::instance().debug(
			"Forcing __postInit: constructor performs external calls (inner "
			"txns need resource population unavailable in the create txn)");
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
	if (detectAsmInConstructor(_contract))
	{
		Logger::instance().debug("Forcing __postInit: constructor/state-init reaches inline assembly (storage dispatcher may create boxes)");
		return true;
	}
	return false;
}

} // namespace puyasol::builder
