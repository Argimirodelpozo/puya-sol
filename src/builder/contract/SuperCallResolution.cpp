/// @file SuperCallResolution.cpp
/// super.f() / Base.f() target resolution and subroutine emission for
/// ContractBuilder — via solc's OWN virtual-dispatch resolver
/// (fable-review.md item 11).
///
/// solc annotates every member access with `requiredLookup`
/// (Static / Virtual / Super) and provides the language-definition resolver
/// `FunctionDefinition::resolveVirtual(mostDerived, searchStart)`: with
/// searchStart = the contract CONTAINING the call site, it returns the next
/// implementation after that contract in mostDerived's C3 linearization —
/// exactly `super.f()` semantics (this is what solc's own code generators
/// call). Explicit `Base.f()` is Static: the referenced declaration IS the
/// target. This replaces the previous hand-rolled resolver (name-keyed MRO
/// chains + an AST-id fallback + a separate explicit-base path), which
/// mixed overloads into one chain and resolved cross-function super calls
/// through a non-MRO fallback.
///
///   collectSuperCallMetadata — resolve every super/base call site across
///                              the linearized hierarchy (methods + ctors).
///   applySuperOverridesFor    — wire a caller's resolved targets into
///                              ContractContext before translating its body.
///   clearSuperOverrides       — wipe between function translations.
///   emitSuperSubroutines      — emit each resolved TARGET once, as
///                              f<suffix>__impl_<targetId> (deduped across
///                              callers; the old scheme emitted one copy per
///                              caller).
///
/// Constructor bodies (most-derived AND inlined base ctors) translate inside
/// buildApprovalProgram against the m_allSuperTargetNames SNAPSHOT taken
/// right after collection — so sites discovered in ANY constructor body are
/// registered eagerly at collect time (the snapshot is a flat map; a
/// refDecl-id collision between two ctor scopes remains a pre-existing
/// limitation). Method-body sites are applied strictly per caller.

#include "builder/contract/ContractBuilder.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/sol-types/OverloadSuffix.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

namespace
{

/// One resolved super/base call site.
struct CallSite
{
	solidity::frontend::FunctionDefinition const* refDecl; ///< the annotation's declaration (call-site key)
	bool isSuper;                                          ///< Super (MRO) vs Static explicit Base.f()
};

/// Collect super.f() and explicit Base.f() call sites from one body.
/// Uses solc's requiredLookup: Super = MRO-dependent, Static on a contract
/// TypeType = explicit base, Virtual = normal dispatch (handled by regular
/// most-derived method emission, not here).
class SiteCollector: public solidity::frontend::ASTConstVisitor
{
public:
	std::vector<CallSite> sites;

	bool visit(solidity::frontend::MemberAccess const& _node) override
	{
		auto const* funcDecl = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
			_node.annotation().referencedDeclaration);
		if (!funcDecl || !_node.annotation().requiredLookup.set())
			return true;

		using solidity::frontend::VirtualLookup;
		switch (*_node.annotation().requiredLookup)
		{
		case VirtualLookup::Super:
			sites.push_back({funcDecl, true});
			break;
		case VirtualLookup::Static:
		{
			// Only explicit-base calls where the base is a contract TypeType;
			// exclude Lib.f() explicitly: solc models libraries as ContractType
			// too, so checking only the type category misclassified every static
			// library call as a base call and emitted a duplicate __impl method.
			if (auto const* scope = funcDecl->annotation().contract;
				scope && scope->isLibrary())
				break;
			auto const* baseType = _node.expression().annotation().type;
			if (!baseType
				|| baseType->category() != solidity::frontend::Type::Category::TypeType)
				break;
			auto const* tt = dynamic_cast<solidity::frontend::TypeType const*>(baseType);
			if (tt
				&& tt->actualType()
				&& tt->actualType()->category() == solidity::frontend::Type::Category::Contract)
				sites.push_back({funcDecl, false});
			break;
		}
		case VirtualLookup::Virtual:
			break;
		}
		return true;
	}
};

} // anonymous namespace

std::string ContractBuilder::superImplName(
	solidity::frontend::FunctionDefinition const& _target) const
{
	std::string name = _target.name();
	if (m_overloadedNames.count(name))
		name += paramCountSuffix(_target);
	return name + "__impl_" + std::to_string(_target.id());
}

void ContractBuilder::collectSuperCallMetadata(
	solidity::frontend::ContractDefinition const& _contract)
{
	m_perFuncSuperOverrides.clear();
	m_superImplsToEmit.clear();

	// Resolve every call site in one body; register per caller, and eagerly
	// (into the translator, for the ctor snapshot) when requested.
	auto processSites = [&](
		solidity::frontend::FunctionDefinition const& _scopeFn,
		solidity::frontend::ContractDefinition const& _scopeContract,
		solidity::frontend::Block const& _body,
		bool _eager)
	{
		SiteCollector collector;
		_body.accept(collector);
		for (auto const& site: collector.sites)
		{
			auto const* target = site.refDecl;
			if (site.isSuper)
			{
				// The language-definition super resolution. resolveVirtual's
				// _searchStart is INCLUSIVE, so — exactly like solc's own
				// codegen — the search starts at the scope contract's
				// SUCCESSOR in _contract's linearization
				// (ContractDefinition::superContract).
				auto const* searchStart = _scopeContract.superContract(_contract);
				if (!searchStart)
					continue; // scope is last in the linearization; nothing above
				target = &site.refDecl->resolveVirtual(_contract, searchStart);
			}
			if (!target->isImplemented())
				continue;

			std::string subName = superImplName(*target);
			m_perFuncSuperOverrides[_scopeFn.id()].push_back({site.refDecl->id(), subName});
			m_superImplsToEmit[target->id()] = target;
			if (_eager)
				m_tr->setSuperTarget(site.refDecl->id(), subName);
		}
	};

	// A function executes the bodies of its applied modifiers too. Resolve the
	// actual virtual modifier exactly as modifier lowering does, then attach
	// every super/base call found there to the enclosing function's override map.
	auto processFunction = [&](solidity::frontend::FunctionDefinition const& fn,
		solidity::frontend::ContractDefinition const& scope, bool eager)
	{
		if (!fn.isImplemented()) return;
		processSites(fn, scope, fn.body(), eager);
		for (auto const& invocation: fn.modifiers())
		{
			auto const* mod = dynamic_cast<solidity::frontend::ModifierDefinition const*>(
				invocation->name().annotation().referencedDeclaration);
			if (!mod) continue; // base-constructor invocation
			bool explicitBase = invocation->name().path().size() > 1;
			if (!explicitBase)
			{
				solidity::frontend::ModifierDefinition const* resolved = nullptr;
				forEachFunctionModifier(_contract, [&](auto const* candidate) {
					if (!resolved && candidate->name() == mod->name())
						resolved = candidate;
				});
				if (resolved) mod = resolved;
			}
			auto const* modScope = mod->annotation().contract;
			if (modScope)
				processSites(fn, *modScope, mod->body(), eager);
		}
	};

	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		for (auto const* func: base->definedFunctions())
			if (!func->isConstructor())
				processFunction(*func, *base, /*_eager=*/false);
		if (auto const* ctor = base->constructor())
			processFunction(*ctor, *base, /*_eager=*/true);
	}
}

void ContractBuilder::applySuperOverridesFor(int64_t _callerFuncId)
{
	auto it = m_perFuncSuperOverrides.find(_callerFuncId);
	if (it != m_perFuncSuperOverrides.end())
		for (auto const& [refDeclId, subName]: it->second)
			m_tr->setSuperTarget(refDeclId, subName);
}

void ContractBuilder::clearSuperOverrides()
{
	m_tr->clearSuperTargets();
}

void ContractBuilder::emitSuperSubroutines(
	awst::Contract& _contractNode,
	std::string const& _contractName)
{
	// One subroutine per resolved TARGET implementation, shared by every
	// caller. The target's own body may contain super/base calls — its
	// per-caller overrides were registered by collectSuperCallMetadata
	// (every implemented function in the hierarchy is scanned), so applying
	// them here resolves chained supers recursively.
	for (auto const& [targetId, func]: m_superImplsToEmit)
	{
		clearSuperOverrides();
		applySuperOverridesFor(targetId);
		// Internal copy: the impl is a direct callsub target. Building it as
		// an ABI method and resetting the config AFTERWARDS left the callee's
		// entry semantics baked into the body — a payable caller inherited
		// the base's not-payable group assert and falsely reverted.
		auto method = buildFunction(
			*func, _contractName, superImplName(*func), /*_asInternalCopy=*/true);
		_contractNode.methods.push_back(std::move(method));
		// A super TARGET with modifiers builds its viaIR modifier chain
		// (`f__mod{i}_N` subroutines) into m_modifierSubroutines; flush them into the
		// contract like every other function-emission site (ContractBuilder.cpp:426 etc.).
		// Without this the super copy's body references an unemitted `f__mod0_N` →
		// puya "unable to resolve function reference" (viaIR only; legacy inlines
		// modifiers in-body so m_modifierSubroutines stays empty). (Found by fuzz_dispatch.)
		for (auto& sub: m_modifierSubroutines)
			_contractNode.methods.push_back(std::move(sub));
		m_modifierSubroutines.clear();
	}
}

} // namespace puyasol::builder
