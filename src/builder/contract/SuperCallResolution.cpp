/// @file SuperCallResolution.cpp
/// MRO-aware super.f() / Base.f() target resolution and subroutine emission
/// for ContractBuilder. Splits out of ContractBuilder::build():
///
///   collectSuperCallMetadata — find every super-call target across the
///                              contract's linearized base hierarchy.
///   applySuperOverridesFor    — wire the per-caller super names into
///                              ContractContext before translating a body.
///   clearSuperOverrides       — wipe between function translations.
///   emitSuperSubroutines      — generate the f__super_<callerId> bodies
///                              once all regular methods are built.

#include "builder/contract/ContractBuilder.h"
#include "builder/sol-types/OverloadSuffix.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <set>

namespace puyasol::builder
{

namespace
{

/// Collect base-function AST IDs from super.f() (MRO) and Base.f() (fixed).
/// Uses requiredLookup: Super=MRO-dependent, Static=explicit-base, Virtual=router.
class SuperCallCollector: public solidity::frontend::ASTConstVisitor
{
public:
	std::set<int64_t> superTargetIds;       ///< MRO-dependent (super.f())
	std::set<int64_t> explicitBaseTargetIds; ///< Fixed (Base.f())

	bool visit(solidity::frontend::MemberAccess const& _node) override
	{
		auto const* refDecl = _node.annotation().referencedDeclaration;
		if (!refDecl)
			return true;
		if (!_node.annotation().requiredLookup.set())
			return true;

		using solidity::frontend::VirtualLookup;
		switch (*_node.annotation().requiredLookup)
		{
		case VirtualLookup::Super:
			superTargetIds.insert(refDecl->id());
			break;
		case VirtualLookup::Static:
		{
			// Only count explicit-base when the base is a contract TypeType;
			// excludes Lib.f() and other static cases.
			auto const* baseType = _node.expression().annotation().type;
			if (!baseType
				|| baseType->category() != solidity::frontend::Type::Category::TypeType)
				break;
			auto const* tt = dynamic_cast<solidity::frontend::TypeType const*>(baseType);
			if (tt
				&& tt->actualType()
				&& tt->actualType()->category() == solidity::frontend::Type::Category::Contract)
				explicitBaseTargetIds.insert(refDecl->id());
			break;
		}
		case VirtualLookup::Virtual:
			break;
		}
		return true;
	}
};

} // anonymous namespace

void ContractBuilder::collectSuperCallMetadata(
	solidity::frontend::ContractDefinition const& _contract)
{
	// MRO-aware: super.f() in X calls the NEXT f in the most-derived MRO.
	// D is B,C: MRO=D→C→B→A; C.super.f() → B.f(), not A.f().
	auto const& mro = _contract.annotation().linearizedBaseContracts;
	struct MroChainEntry {
		solidity::frontend::ContractDefinition const* contract;
		solidity::frontend::FunctionDefinition const* func;
	};
	std::map<std::string, std::vector<MroChainEntry>> mroChains; // funcName → MRO order
	for (auto const* base: mro)
		for (auto const* func: base->definedFunctions())
			if (!func->isConstructor() && func->isImplemented())
				mroChains[func->name()].push_back({base, func});

	// Build callerFuncId → (superName, targetFunc) for per-context super resolution.
	m_superTargetFuncs.clear();
	m_perFuncSuperOverrides.clear();

	for (auto const& [fname, chain]: mroChains)
	{
		for (size_t i = 0; i < chain.size(); ++i)
		{
			SuperCallCollector funcCollector;
			chain[i].func->body().accept(funcCollector);
			if (funcCollector.superTargetIds.empty())
				continue;
			// super.f() → chain[i+1]
			if (i + 1 >= chain.size())
				continue;

			auto const* mroTarget = chain[i + 1].func;
			int64_t callerId = chain[i].func->id();

			for (int64_t superCallTargetId: funcCollector.superTargetIds)
			{
				std::string name = fname;
				if (m_overloadedNames.count(name))
					name += paramCountSuffix(*mroTarget);
				std::string superName = name + "__super_" + std::to_string(callerId);

				m_perFuncSuperOverrides[callerId].push_back({superCallTargetId, superName});
				m_superTargetFuncs[callerId] = mroTarget;
			}
		}

		// Constructor super.f(): ctor sits above chain[0] in MRO.
		// Target = first f in the super.f() ref; looked up via m_perFuncSuperOverrides.
		if (auto const* ctor = _contract.constructor())
		{
			if (ctor->isImplemented())
			{
				SuperCallCollector ctorCollector;
				ctor->body().accept(ctorCollector);
				for (int64_t superCallTargetId: ctorCollector.superTargetIds)
				{
					solidity::frontend::FunctionDefinition const* target = nullptr;
					for (auto const& entry: chain)
						if (entry.func->id() == superCallTargetId)
						{ target = entry.func; break; }
					if (!target)
						continue;

					int64_t callerId = ctor->id();
					std::string name = fname;
					if (m_overloadedNames.count(name))
						name += paramCountSuffix(*target);
					std::string superName = name + "__super_" + std::to_string(callerId);
					m_perFuncSuperOverrides[callerId].push_back({superCallTargetId, superName});
					m_superTargetFuncs[callerId] = target;
					m_tr->setSuperTarget(superCallTargetId, superName);
				}
			}
		}
	}

	auto collectAllSuperCalls = [&](SuperCallCollector& c) {
		for (auto const* base: mro)
			for (auto const* func: base->definedFunctions())
				if (func->isImplemented())
					func->body().accept(c);
	};

	// Fallback: super.f() from a different-named function (g → super.f());
	// MRO chain doesn't cover it — fall back to AST-ID-based resolution.
	m_fallbackSuperFuncs.clear();
	{
		SuperCallCollector globalCollector;
		collectAllSuperCalls(globalCollector);

		std::set<int64_t> handledSuperIds;
		for (auto const& [callerId, overrides]: m_perFuncSuperOverrides)
			for (auto const& [targetId, name]: overrides)
				handledSuperIds.insert(targetId);

		for (int64_t id: globalCollector.superTargetIds)
		{
			if (handledSuperIds.count(id))
				continue;

			for (auto const* base: mro)
			{
				for (auto const* func: base->definedFunctions())
				{
					if (func->id() == id && func->isImplemented())
					{
						m_fallbackSuperFuncs[id] = func;
						std::string name = func->name();
						if (m_overloadedNames.count(name))
							name += "_" + std::to_string(func->parameters().size());
						std::string superName = name + "__super_" + std::to_string(id);
						m_tr->setSuperTarget(id, superName);
					}
				}
			}
		}
	}

	// Explicit Base.f() calls: fixed target regardless of MRO.
	m_explicitBaseTargetFuncs.clear();
	{
		SuperCallCollector globalCollector;
		collectAllSuperCalls(globalCollector);

		for (int64_t id: globalCollector.explicitBaseTargetIds)
		{
			for (auto const* base: mro)
			{
				for (auto const* func: base->definedFunctions())
				{
					if (func->id() == id && func->isImplemented())
					{
						m_explicitBaseTargetFuncs[id] = func;
						std::string name = func->name();
						if (m_overloadedNames.count(name))
							name += paramCountSuffix(*func);
						std::string superName = name + "__super_" + std::to_string(id);
						m_tr->setSuperTarget(id, superName);
					}
				}
			}
		}
	}
}

void ContractBuilder::applySuperOverridesFor(int64_t _callerFuncId)
{
	auto it = m_perFuncSuperOverrides.find(_callerFuncId);
	if (it != m_perFuncSuperOverrides.end())
		for (auto const& [targetId, superName]: it->second)
			m_tr->setSuperTarget(targetId, superName);
	for (auto const& [id, func]: m_fallbackSuperFuncs)
	{
		std::string name = func->name();
		if (m_overloadedNames.count(name))
			name += "_" + std::to_string(func->parameters().size());
		m_tr->setSuperTarget(id, name + "__super_" + std::to_string(id));
	}
	for (auto const& [id, func]: m_explicitBaseTargetFuncs)
	{
		std::string name = func->name();
		if (m_overloadedNames.count(name))
			name += paramCountSuffix(*func);
		m_tr->setSuperTarget(id, name + "__super_" + std::to_string(id));
	}
}

void ContractBuilder::clearSuperOverrides()
{
	m_tr->clearSuperTargets();
}

void ContractBuilder::emitSuperSubroutines(
	awst::Contract& _contractNode,
	std::string const& _contractName)
{
	for (auto const& [callerFuncId, targetFunc]: m_superTargetFuncs)
	{
		std::string name = targetFunc->name();
		if (m_overloadedNames.count(name))
			name += paramCountSuffix(*targetFunc);
		std::string superName = name + "__super_" + std::to_string(callerFuncId);
		clearSuperOverrides();
		applySuperOverridesFor(targetFunc->id());
		auto method = buildFunction(*targetFunc, _contractName, superName);
		method.arc4MethodConfig.reset();
		_contractNode.methods.push_back(std::move(method));
	}

	// Fallback super subroutines.
	for (auto const& [targetId, func]: m_fallbackSuperFuncs)
	{
		std::string name = func->name();
		if (m_overloadedNames.count(name))
			name += "_" + std::to_string(func->parameters().size());
		std::string superName = name + "__super_" + std::to_string(targetId);
		clearSuperOverrides();
		auto method = buildFunction(*func, _contractName, superName);
		method.arc4MethodConfig.reset();
		_contractNode.methods.push_back(std::move(method));
	}

	// Explicit Base.f() subroutines.
	for (auto const& [targetId, func]: m_explicitBaseTargetFuncs)
	{
		std::string name = func->name();
		if (m_overloadedNames.count(name))
			name += paramCountSuffix(*func);
		std::string superName = name + "__super_" + std::to_string(targetId);
		clearSuperOverrides();
		auto method = buildFunction(*func, _contractName, superName);
		method.arc4MethodConfig.reset();
		_contractNode.methods.push_back(std::move(method));
	}
}

} // namespace puyasol::builder
