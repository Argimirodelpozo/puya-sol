#include "builder/contract/PostInitTriggers.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <functional>
#include <deque>
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

/// Constructor effects also occur while evaluating explicit base-constructor
/// arguments.  Those expressions live on inheritance specifiers and
/// constructor modifier invocations, outside every constructor body.
template <class V>
void walkAllBaseConstructorArgs(
	solidity::frontend::ContractDefinition const& _contract,
	V& _checker,
	std::function<bool()> const& _done)
{
	for (auto const* level: _contract.annotation().linearizedBaseContracts)
	{
		if (!level) continue;
		for (auto const& spec: level->baseContracts())
			if (auto const* args = spec->arguments())
				for (auto const& arg: *args)
					if (arg)
					{
						arg->accept(_checker);
						if (_done()) return;
					}
		if (auto const* ctor = level->constructor())
			for (auto const& mod: ctor->modifiers())
			{
				if (!dynamic_cast<solidity::frontend::ContractDefinition const*>(
						mod->name().annotation().referencedDeclaration))
					continue;
				if (auto const* args = mod->arguments())
					for (auto const& arg: *args)
						if (arg)
						{
							arg->accept(_checker);
							if (_done()) return;
						}
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

/// Apply `_checker` to constructor roots and every locally executed callable
/// reachable from them.  This is a real call-closure walk (cycle guarded), not
/// a one-hop "functions containing X" set: ctor -> f -> g -> X and deeper
/// chains are equivalent to a direct constructor effect.
template <class V>
bool walkConstructorCallClosure(
	solidity::frontend::ContractDefinition const& _contract,
	V& _checker,
	std::function<bool()> const& _done)
{
	using namespace solidity::frontend;
	std::deque<FunctionDefinition const*> functions;
	std::deque<ModifierDefinition const*> modifiers;
	std::set<int64_t> seenFunctions;
	std::set<int64_t> seenModifiers;

	auto isThisCall = [](FunctionCall const& _call) {
		Expression const* callee = &_call.expression();
		if (auto const* opts = dynamic_cast<FunctionCallOptions const*>(callee))
			callee = &opts->expression();
		auto const* member = dynamic_cast<MemberAccess const*>(callee);
		if (!member)
			return false;
		Expression const* receiver = &member->expression();
		for (;;)
		{
			if (auto const* conversion = dynamic_cast<FunctionCall const*>(receiver);
				conversion && conversion->annotation().kind.set()
				&& *conversion->annotation().kind == FunctionCallKind::TypeConversion
				&& conversion->arguments().size() == 1)
			{
				receiver = conversion->arguments()[0].get();
				continue;
			}
			if (auto const* tuple = dynamic_cast<TupleExpression const*>(receiver);
				tuple && tuple->components().size() == 1 && tuple->components()[0])
			{
				receiver = tuple->components()[0].get();
				continue;
			}
			break;
		}
		auto const* id = dynamic_cast<Identifier const*>(receiver);
		return id && id->name() == "this";
	};

	struct CallCollector: ASTConstVisitor
	{
		std::deque<FunctionDefinition const*>& pending;
		std::function<bool(FunctionCall const&)> const& isThis;
		CallCollector(
			std::deque<FunctionDefinition const*>& _pending,
			std::function<bool(FunctionCall const&)> const& _isThis)
			: pending(_pending), isThis(_isThis) {}
		bool visit(FunctionCall const& _call) override
		{
			auto const* fn = dynamic_cast<FunctionDefinition const*>(
				ASTNode::referencedDeclaration(_call.expression()));
			if (!fn || !fn->isImplemented())
				return true;
			auto const* ft = dynamic_cast<FunctionType const*>(
				_call.expression().annotation().type);
			if (!ft)
				return true;
			using K = FunctionType::Kind;
			bool local = ft->kind() == K::Internal
				|| ft->kind() == K::DelegateCall
				|| (ft->kind() == K::External && isThis(_call));
			if (local)
				pending.push_back(fn);
			return true;
		}
	};
	std::function<bool(FunctionCall const&)> isThisFn = isThisCall;
	CallCollector calls(functions, isThisFn);

	auto scanRoots = [&](auto& visitor) {
		walkAllStateVarInits(_contract, visitor);
		walkAllConstructors(_contract, visitor, [] { return false; });
		walkAllBaseConstructorArgs(_contract, visitor, [] { return false; });
	};
	scanRoots(_checker);
	if (_done())
		return true;
	scanRoots(calls);

	auto enqueueModifiers = [&](FunctionDefinition const& _fn) {
		for (auto const& invocation: _fn.modifiers())
		{
			if (auto const* args = invocation->arguments())
				for (auto const& arg: *args)
					if (arg)
					{
						arg->accept(_checker);
						if (_done()) return;
						arg->accept(calls);
					}
			if (auto const* modifier = dynamic_cast<ModifierDefinition const*>(
					invocation->name().annotation().referencedDeclaration))
				modifiers.push_back(modifier);
		}
	};
	for (auto const* level: _contract.annotation().linearizedBaseContracts)
		if (level)
			if (auto const* ctor = level->constructor())
				enqueueModifiers(*ctor);
	if (_done())
		return true;

	while (!functions.empty() || !modifiers.empty())
	{
		while (!functions.empty())
		{
			auto const* fn = functions.front();
			functions.pop_front();
			if (!fn || !seenFunctions.insert(fn->id()).second)
				continue;
			enqueueModifiers(*fn);
			if (_done()) return true;
			fn->body().accept(_checker);
			if (_done()) return true;
			fn->body().accept(calls);
		}
		while (!modifiers.empty())
		{
			auto const* modifier = modifiers.front();
			modifiers.pop_front();
			if (!modifier || !modifier->isImplemented()
				|| !seenModifiers.insert(modifier->id()).second)
				continue;
			modifier->body().accept(_checker);
			if (_done()) return true;
			modifier->body().accept(calls);
		}
	}
	return _done();
}

/// True if the constructor (directly or via a function call) touches box-stored state.
bool detectBoxRefsInConstructor(
	solidity::frontend::ContractDefinition const& _contract,
	StorageMapper const& _storageMapper)
{
	std::set<int64_t> boxVarIds;
	forEachStateVar(_contract, [&](auto const* var)
	{
		if (var->isConstant())
			return;
		if (_storageMapper.shouldUseBoxStorage(*var))
			boxVarIds.insert(var->id());
	});
	if (boxVarIds.empty())
		return false;

	BoxVarRefChecker checker(boxVarIds);
	return walkConstructorCallClosure(
		_contract, checker, [&]{ return checker.found(); });
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
	return walkConstructorCallClosure(
		_contract, checker, [&]{ return checker.found; });
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
	return walkConstructorCallClosure(
		_contract, checker, [&]{ return checker.found; });
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
	return walkConstructorCallClosure(
		_contract, checker, [&]{ return checker.found; });
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
	AsmDetector checker;
	return walkConstructorCallClosure(
		_contract, checker, [&]{ return checker.found; });
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
	return walkConstructorCallClosure(
		_contract, checker, [&]{ return checker.found; });
}

} // namespace

bool computeNeedsPostInit(
	solidity::frontend::ContractDefinition const& _contract,
	StorageMapper const& _storageMapper)
{
	// --evm-storage-layout: EVERY state write is a box write, and boxes of the
	// app being created cannot be referenced in the create txn — defer any
	// state initializer / constructor body to __postInit.
	if (_storageMapper.profile().evmStorageLayout)
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
	if (detectBoxRefsInConstructor(_contract, _storageMapper))
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
