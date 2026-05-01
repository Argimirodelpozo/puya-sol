/// @file SuperCallResolution.cpp
/// MRO-aware super.f() / Base.f() target resolution and subroutine emission
/// for ContractBuilder. Splits out of ContractBuilder::build():
///
///   collectSuperCallMetadata — find every super-call target across the
///                              contract's linearized base hierarchy.
///   applySuperOverridesFor    — wire the per-caller super names into
///                              BuilderContext before translating a body.
///   clearSuperOverrides       — wipe between function translations.
///   emitSuperSubroutines      — generate the f__super_<callerId> bodies
///                              once all regular methods are built.

#include "builder/ContractBuilder.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <set>

namespace puyasol::builder
{

namespace
{

/// AST visitor: collect AST IDs of base functions reached via `super.f()`
/// (MRO-dependent) or `Base.f()` (fixed target).
class SuperCallCollector: public solidity::frontend::ASTConstVisitor
{
public:
	std::set<int64_t> superTargetIds;       ///< MRO-dependent (super.f())
	std::set<int64_t> explicitBaseTargetIds; ///< Fixed (Base.f())

	bool visit(solidity::frontend::MemberAccess const& _node) override
	{
		auto const* baseType = _node.expression().annotation().type;
		if (!baseType)
			return true;
		// Unwrap TypeType (super has type TypeType(ContractType(isSuper=true)))
		if (baseType->category() == solidity::frontend::Type::Category::TypeType)
		{
			auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType);
			if (typeType)
				baseType = typeType->actualType();
		}
		if (baseType->category() == solidity::frontend::Type::Category::Contract)
		{
			auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(baseType);
			if (contractType)
			{
				auto const* refDecl = _node.annotation().referencedDeclaration;
				if (!refDecl)
					return true;

				if (contractType->isSuper())
				{
					// super.f() — MRO-dependent resolution
					superTargetIds.insert(refDecl->id());
				}
				else
				{
					// Explicit Base.f() — always calls the specific base version
					bool isExplicitBase = _node.expression().annotation().type
						&& _node.expression().annotation().type->category()
							== solidity::frontend::Type::Category::TypeType;
					if (isExplicitBase)
						explicitBaseTargetIds.insert(refDecl->id());
				}
			}
		}
		return true;
	}
};

} // anonymous namespace

void ContractBuilder::collectSuperCallMetadata(
	solidity::frontend::ContractDefinition const& _contract)
{
	// Scan all functions (own + inherited) for super.method() calls.
	// Collect AST IDs of base functions that need separate subroutines.
	// MRO-aware super resolution: super.f() in contract X calls the NEXT
	// implementation of f in the most-derived contract's MRO, not X's own base.
	// E.g., for D is B,C: MRO = D→C→B→A. C.super.f() should call B.f(), not A.f().

	// Build per-function-name MRO chains
	auto const& mro = _contract.annotation().linearizedBaseContracts;
	struct MroChainEntry {
		solidity::frontend::ContractDefinition const* contract;
		solidity::frontend::FunctionDefinition const* func;
	};
	// funcName → ordered chain of implementations in MRO order
	std::map<std::string, std::vector<MroChainEntry>> mroChains;
	for (auto const* base: mro)
		for (auto const* func: base->definedFunctions())
			if (!func->isConstructor() && func->isImplemented())
				mroChains[func->name()].push_back({base, func});

	// For each function in the MRO chain, build (callerFuncId → superSubroutineName, targetFunc).
	// This allows per-calling-context super resolution.
	m_superTargetFuncs.clear();
	m_perFuncSuperOverrides.clear();

	for (auto const& [fname, chain]: mroChains)
	{
		for (size_t i = 0; i < chain.size(); ++i)
		{
			// Check if this function has super.f() calls
			SuperCallCollector funcCollector;
			chain[i].func->body().accept(funcCollector);
			if (funcCollector.superTargetIds.empty())
				continue;

			// This function calls super → target is chain[i+1]
			if (i + 1 >= chain.size())
				continue;

			auto const* mroTarget = chain[i + 1].func;
			int64_t callerId = chain[i].func->id();

			for (int64_t superCallTargetId: funcCollector.superTargetIds)
			{
				std::string name = fname;
				if (m_overloadedNames.count(name))
					name += "(" + std::to_string(mroTarget->parameters().size()) + ")";
				std::string superName = name + "__super_" + std::to_string(callerId);

				m_perFuncSuperOverrides[callerId].push_back({superCallTargetId, superName});
				m_superTargetFuncs[callerId] = mroTarget;
			}
		}

		// Constructor super.f(): the most-derived contract's constructor
		// sits conceptually one position above chain[0] in the MRO. Its
		// super target is whichever `f` appears first in the constructor
		// body's super.f() reference. Emit a caller-keyed subroutine whose
		// body is that target — the constructor call site will look up
		// `f__super_<ctor.id>` via m_perFuncSuperOverrides.
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
						name += "(" + std::to_string(target->parameters().size()) + ")";
					std::string superName = name + "__super_" + std::to_string(callerId);
					m_perFuncSuperOverrides[callerId].push_back({superCallTargetId, superName});
					m_superTargetFuncs[callerId] = target;
					m_exprBuilder->superTargetNames[superCallTargetId] = superName;
				}
			}
		}
	}

	// Fallback: super calls not handled by MRO chains (e.g., g() calling super.f()
	// where g and f are different function names). Use original AST-ID-based resolution.
	m_fallbackSuperFuncs.clear();
	{
		SuperCallCollector globalCollector;
		for (auto const* base: mro)
			for (auto const* func: base->definedFunctions())
				if (func->isImplemented())
					func->body().accept(globalCollector);

		// Collect all super target IDs already handled by MRO chain
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
						m_exprBuilder->superTargetNames[id] = superName;
					}
				}
			}
		}
	}

	// Handle explicit base class calls (Base.f() — NOT super.f()).
	// These always resolve to the specific base function, regardless of MRO.
	m_explicitBaseTargetFuncs.clear();
	{
		SuperCallCollector globalCollector;
		for (auto const* base: mro)
			for (auto const* func: base->definedFunctions())
				if (func->isImplemented())
					func->body().accept(globalCollector);

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
							name += "(" + std::to_string(func->parameters().size()) + ")";
						std::string superName = name + "__super_" + std::to_string(id);
						// Register globally — explicit base calls don't need per-function context
						m_exprBuilder->superTargetNames[id] = superName;
					}
				}
			}
		}
	}
}

void ContractBuilder::applySuperOverridesFor(int64_t _callerFuncId)
{
	// MRO-dependent overrides for this specific function
	auto it = m_perFuncSuperOverrides.find(_callerFuncId);
	if (it != m_perFuncSuperOverrides.end())
		for (auto const& [targetId, superName]: it->second)
			m_exprBuilder->superTargetNames[targetId] = superName;
	// Re-register fallback super targets (cross-function super calls)
	for (auto const& [id, func]: m_fallbackSuperFuncs)
	{
		std::string name = func->name();
		if (m_overloadedNames.count(name))
			name += "_" + std::to_string(func->parameters().size());
		m_exprBuilder->superTargetNames[id] = name + "__super_" + std::to_string(id);
	}
	// Re-register explicit base targets (they're fixed, not MRO-dependent)
	for (auto const& [id, func]: m_explicitBaseTargetFuncs)
	{
		std::string name = func->name();
		if (m_overloadedNames.count(name))
			name += "(" + std::to_string(func->parameters().size()) + ")";
		m_exprBuilder->superTargetNames[id] = name + "__super_" + std::to_string(id);
	}
}

void ContractBuilder::clearSuperOverrides()
{
	m_exprBuilder->superTargetNames.clear();
}

void ContractBuilder::emitSuperSubroutines(
	awst::Contract& _contractNode,
	std::string const& _contractName)
{
	// MRO-dependent super subroutines (keyed by caller func AST ID)
	for (auto const& [callerFuncId, targetFunc]: m_superTargetFuncs)
	{
		std::string name = targetFunc->name();
		if (m_overloadedNames.count(name))
			name += "(" + std::to_string(targetFunc->parameters().size()) + ")";
		std::string superName = name + "__super_" + std::to_string(callerFuncId);
		clearSuperOverrides();
		applySuperOverridesFor(targetFunc->id());
		auto method = buildFunction(*targetFunc, _contractName, superName);
		method.arc4MethodConfig.reset();
		_contractNode.methods.push_back(std::move(method));
	}

	// Fallback super subroutines (cross-function super calls)
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

	// Explicit base class call subroutines (keyed by target func AST ID)
	for (auto const& [targetId, func]: m_explicitBaseTargetFuncs)
	{
		std::string name = func->name();
		if (m_overloadedNames.count(name))
			name += "(" + std::to_string(func->parameters().size()) + ")";
		std::string superName = name + "__super_" + std::to_string(targetId);
		clearSuperOverrides();
		auto method = buildFunction(*func, _contractName, superName);
		method.arc4MethodConfig.reset();
		_contractNode.methods.push_back(std::move(method));
	}
}

} // namespace puyasol::builder
