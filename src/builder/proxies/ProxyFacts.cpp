#include "builder/proxies/ProxyFacts.h"
#include "builder/SourceLocConvert.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/ast/TypeProvider.h>
#include <libsolidity/ast/Types.h>
#include <algorithm>
#include <iterator>
#include <sstream>

namespace puyasol::builder::proxies
{
namespace
{
using namespace solidity::frontend;

bool signature(FunctionDefinition const& function,
	std::vector<Type const*> const& parameters, std::vector<Type const*> const& returns,
	std::initializer_list<Visibility> visibility, std::initializer_list<StateMutability> mutability)
{
	auto matches = [](auto const& declarations, auto const& types) {
		if (declarations.size() != types.size()) return false;
		for (size_t i = 0; i < types.size(); ++i)
			if (*declarations[i]->type() != *types[i]) return false;
		return true;
	};
	return matches(function.parameters(), parameters) && matches(function.returnParameters(), returns)
		&& std::find(visibility.begin(), visibility.end(), function.visibility()) != visibility.end()
		&& std::find(mutability.begin(), mutability.end(), function.stateMutability()) != mutability.end();
}

// Conservatively require the hook's implementation argument to be unreferenced.
// This includes modifier arguments, super/helper forwarding and inline Yul;
// declarations are compared by solc identity, not by their spelling. No invented
// native "new implementation" value can satisfy an implementation-dependent hook.
bool consumesImplementation(FunctionDefinition const& function)
{
	struct References: ASTConstVisitor
	{
		VariableDeclaration const& parameter;
		bool used = false;
		explicit References(VariableDeclaration const& p): parameter(p) {}
		bool visit(Identifier const& id) override
		{
			used |= id.annotation().referencedDeclaration == &parameter;
			return true;
		}
		bool visit(InlineAssembly const& assembly) override
		{
			for (auto const& [_, reference]: assembly.annotation().externalReferences)
				used |= reference.declaration == &parameter;
			return false;
		}
	} references(*function.parameters().at(0));
	function.accept(references);
	return references.used;
}
} // namespace

ProxyFacts ProxyFacts::analyze(
	std::vector<ContractDefinition const*> const& contracts, SourceMap const& sources)
{
	using V = Visibility;
	using M = StateMutability;
	using F = Erc1967UtilsFold;
	ProxyFacts result;
	std::map<int64_t, FunctionDefinition const*> bases;
	auto error = [&](ASTNode const& node, std::string const& message) {
		Logger::instance().error("proxy adaptation: " + message, sources.toAwstLoc("", node.location()));
	};
	for (auto const* contract: contracts)
	{
		auto const& tags = contract->annotation().docTags;
		auto [begin, end] = tags.equal_range("custom:avm-proxy");
		if (begin == end) continue;
		std::string role, extra;
		std::istringstream tag(begin->second.content);
		tag >> role;
		if (std::next(begin) != end || (tag >> extra)
			|| (role != "uups" && role != "erc1967-utils" && role != "proxy")
			|| (contract->isLibrary() != (role == "erc1967-utils")) || contract->isInterface())
		{
			error(*contract, "@custom:avm-proxy requires one role: uups, erc1967-utils (library), or proxy.");
			continue;
		}
		for (auto const* function: contract->definedFunctions())
		{
			auto const& name = function->name();
			bool recognized = false, valid = false;
			UupsFold uups = UupsFold::None;
			F utils = F::None;
			if (role == "uups")
			{
				if (name == "_authorizeUpgrade")
				{
					recognized = true;
					valid = signature(*function, {TypeProvider::address()}, {}, {V::Internal},
						{M::NonPayable, M::View, M::Pure}) && function->virtualSemantics();
					if (valid) bases.emplace(contract->id(), function);
				}
				else if (name == "_checkProxy" || name == "_checkNotDelegated")
				{
					recognized = true;
					valid = signature(*function, {}, {}, {V::Internal}, {M::View, M::Pure});
					uups = UupsFold::EmptyBody;
				}
				else if (name == "upgradeTo" || name == "upgradeToAndCall" || name == "_upgradeToAndCallUUPS")
				{
					recognized = true;
					bool const internal = name == "_upgradeToAndCallUUPS";
					std::vector<Type const*> params{TypeProvider::address()};
					if (name != "upgradeTo") params.push_back(TypeProvider::bytesMemory());
					auto matchesUpgrade = [&] {
						return signature(*function, params, {}, internal
							? std::initializer_list<V>{V::Private, V::Internal}
							: std::initializer_list<V>{V::Public, V::External}, {M::NonPayable, M::Payable});
					};
					valid = matchesUpgrade();
					if (!internal && params.size() == 2)
					{
						params[1] = TypeProvider::bytesCalldata();
						valid |= matchesUpgrade();
					}
					uups = UupsFold::Trap;
				}
			}
			else if (role == "proxy" && name == "_delegate")
			{
				recognized = true;
				valid = signature(*function, {TypeProvider::address()}, {}, {V::Internal}, {M::NonPayable});
				uups = UupsFold::TrapDelegate;
			}
			else if (role == "erc1967-utils")
			{
				bool const load = name == "getImplementation" || name == "getAdmin" || name == "getBeacon";
				bool const store = name == "_setImplementation" || name == "_setAdmin" || name == "_setBeacon";
				bool const upgrade = name == "upgradeToAndCall" || name == "upgradeBeaconToAndCall";
				recognized = load || store || upgrade;
				if (load)
					valid = signature(*function, {}, {TypeProvider::address()}, {V::Internal}, {M::View});
				else if (store)
					valid = signature(*function, {TypeProvider::address()}, {}, {V::Private}, {M::NonPayable});
				else if (upgrade)
					valid = signature(*function, {TypeProvider::address(), TypeProvider::bytesMemory()}, {},
						{V::Internal}, {M::NonPayable});
				utils = name == "getImplementation" ? F::ImplementationLoad
					: name == "getAdmin" ? F::AdminLoad : name == "_setAdmin" ? F::AdminStore
					: name == "_setImplementation" || name == "upgradeToAndCall" ? F::TrapImplementation
					: F::TrapBeacon;
			}
			if (!recognized) continue;
			if (!valid || (name != "_authorizeUpgrade" && !function->isImplemented())
				|| (role == "erc1967-utils" && !function->modifiers().empty()))
			{
				error(*function, "unsupported annotated signature for " + role + "." + name + ".");
				continue;
			}
			if (uups != UupsFold::None) result.uupsFunctions.emplace(function->id(), uups);
			if (utils != F::None) result.utilsFunctions.emplace(function->id(), utils);
		}
		if (role == "uups" && !bases.contains(contract->id()))
			error(*contract, "annotated uups base requires internal virtual _authorizeUpgrade(address).");
	}
	for (auto const* contract: contracts)
	{
		if (contract->isLibrary() || contract->isInterface() || contract->abstract()) continue;
		FunctionDefinition const* hook = nullptr;
		for (auto const* base: contract->annotation().linearizedBaseContracts)
			if (auto found = bases.find(base->id()); found != bases.end())
			{
				auto const* resolved = &found->second->resolveVirtual(*contract);
				if (hook && hook != resolved)
					error(*contract, "multiple annotated UUPS bases resolve to different authorization hooks.");
				hook = resolved;
			}
		if (!hook) continue;
		if (!hook->isImplemented())
			error(*hook, "native update requires an implemented authorization hook.");
		else if (consumesImplementation(*hook))
			error(*hook, "_authorizeUpgrade consumes its implementation argument (including forwarding/modifiers/Yul). "
				"Native updates carry program bytes, not an implementation address; an explicit native authorization adaptation is required.");
		else result.authorizationHooks.emplace(contract->id(), hook);
	}
	return result;
}
} // namespace puyasol::builder::proxies
