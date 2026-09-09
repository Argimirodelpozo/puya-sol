#include "builder/SolcFacts.h"
#include "builder/PreparedAssembly.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>
#include <libsolutil/CommonData.h>
#include <libsolutil/Exceptions.h>
#include <libsolutil/Keccak256.h>
#include <libsolutil/Numeric.h>
#include <libyul/AST.h>
#include <libyul/Dialect.h>
#include <libyul/SideEffects.h>
#include <libyul/optimiser/CallGraphGenerator.h>
#include <libyul/optimiser/ASTWalker.h>
#include <libyul/optimiser/Disambiguator.h>
#include <libyul/optimiser/DataFlowAnalyzer.h>
#include <libyul/optimiser/KnowledgeBase.h>
#include <libyul/optimiser/NameCollector.h>
#include <libyul/optimiser/Semantics.h>
#include <libyul/optimiser/SSAValueTracker.h>

#include <algorithm>
#include <functional>
#include <variant>
#include <vector>

namespace puyasol::builder
{

using namespace solidity::yul;

solidity::frontend::Expression const& SolcFacts::functionExpression(
	solidity::frontend::Expression const& expression)
{
	using namespace solidity::frontend;
	if (auto const* options = dynamic_cast<FunctionCallOptions const*>(&expression))
		return functionExpression(options->expression());
	if (auto const* tuple = dynamic_cast<TupleExpression const*>(&expression);
		tuple && tuple->components().size() == 1 && tuple->components()[0])
		return functionExpression(*tuple->components()[0]);
	return expression;
}

solidity::frontend::FunctionDefinition const* SolcFacts::resolveFunction(
	solidity::frontend::Expression const& expression,
	solidity::frontend::ContractDefinition const* mostDerived)
{
	using namespace solidity::frontend;
	auto const& callee = functionExpression(expression);
	auto const* function = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
		ASTNode::referencedDeclaration(callee));
	if (!function)
		return nullptr;

	// ASTNode::resolveFunctionCall and IRGeneratorForStatements use this same
	// lookup for direct calls and function values. Keep expression identity:
	// modifiers from different bases may name the same referenced declaration.
	if (auto const* member = dynamic_cast<MemberAccess const*>(&callee))
	{
		auto const& lookup = member->annotation().requiredLookup;
		solAssert(lookup.set(), "Missing solc function lookup fact");
		if (*lookup == VirtualLookup::Super)
		{
			auto const* type = dynamic_cast<TypeType const*>(member->expression().annotation().type);
			auto const* owner = type ? dynamic_cast<ContractType const*>(type->actualType()) : nullptr;
			solAssert(mostDerived && owner && owner->isSuper(), "Missing solc super context");
			auto const* start = owner->contractDefinition().superContract(*mostDerived);
			solAssert(start, "Missing solc super successor");
			return &function->resolveVirtual(*mostDerived, start);
		}
		solAssert(*lookup == VirtualLookup::Static, "Unexpected solc member lookup");
	}
	else if (auto const* identifier = dynamic_cast<solidity::frontend::Identifier const*>(&callee))
	{
		solAssert(identifier->annotation().requiredLookup.set()
			&& *identifier->annotation().requiredLookup == VirtualLookup::Virtual,
			"Unexpected solc identifier lookup");
		if (mostDerived && function->virtualSemantics())
			return &function->resolveVirtual(*mostDerived);
	}
	return function;
}

solidity::frontend::FunctionDefinition const* SolcFacts::resolveInternalCall(
	solidity::frontend::FunctionCall const& call,
	solidity::frontend::ContractDefinition const* mostDerived)
{
	using namespace solidity::frontend;
	if (!call.annotation().kind.set()
		|| *call.annotation().kind != FunctionCallKind::FunctionCall)
		return nullptr;
	auto const* type = dynamic_cast<FunctionType const*>(call.expression().annotation().type);
	if (!type || (type->kind() != FunctionType::Kind::Internal
		&& type->kind() != FunctionType::Kind::DelegateCall))
		return nullptr;
	auto const* function = resolveFunction(call.expression(), mostDerived);
	if (function && type->kind() == FunctionType::Kind::DelegateCall
		&& (!function->annotation().contract || !function->annotation().contract->isLibrary()))
		return nullptr;
	return function;
}

solidity::frontend::ModifierDefinition const* SolcFacts::resolveModifier(
	solidity::frontend::ModifierInvocation const& invocation,
	solidity::frontend::ContractDefinition const* mostDerived)
{
	using namespace solidity::frontend;
	auto const* modifier = dynamic_cast<ModifierDefinition const*>(
		invocation.name().annotation().referencedDeclaration);
	if (!modifier)
		return nullptr;
	auto const& lookup = invocation.name().annotation().requiredLookup;
	solAssert(lookup.set(), "Missing solc modifier lookup fact");
	switch (*lookup)
	{
	case VirtualLookup::Static: return modifier;
	case VirtualLookup::Virtual:
		return mostDerived ? &modifier->resolveVirtual(*mostDerived) : modifier;
	case VirtualLookup::Super:
		solAssert(false, "Solc does not permit super modifier lookup");
	}
	return nullptr;
}

namespace
{

std::string nameString(YulName const& _name)
{
	return _name.str();
}

YulName const* userFunctionName(FunctionHandle const& _handle)
{
	return std::get_if<YulName>(&_handle);
}

} // namespace

SolcFacts::YulAnalysis SolcFacts::analyzeYul(
	Block const& _block,
	Dialect const& _dialect)
{
	YulAnalysis result;
	auto definitions = allFunctionDefinitions(_block);
	for (auto const& [name, definition]: definitions)
		result.functions.emplace(nameString(name), definition);

	for (auto const& name: assignedVariableNames(_block))
		result.assignedVariables.insert(nameString(name));

	SSAValueTracker ssaValues;
	ssaValues(_block);
	for (auto const& [name, value]: ssaValues.values())
	{
		if (!value)
			continue;
		auto const* literal = std::get_if<Literal>(value);
		if (!literal || literal->kind != LiteralKind::Number)
			continue;
		result.constantValues.emplace(
			nameString(name), literal->value.value().str());
	}

	auto graph = CallGraphGenerator::callGraph(_block);
	std::set<YulName> recursive;
	for (auto const& handle: graph.recursiveFunctions())
		if (auto const* name = userFunctionName(handle);
			name && definitions.count(*name))
			recursive.insert(*name);

	// Function definitions are declarations, not roots. Traverse only from the
	// empty-name outer context and retain user functions present in this block.
	std::set<YulName> reachable;
	std::vector<YulName> work;
	auto addCallees = [&](YulName const& _caller) {
		auto const found = graph.functionCalls.find(FunctionHandle{_caller});
		if (found == graph.functionCalls.end())
			return;
		for (auto const& callee: found->second)
			if (auto const* name = userFunctionName(callee);
				name && definitions.count(*name) && reachable.insert(*name).second)
				work.push_back(*name);
	};
	addCallees(YulName{});
	for (size_t i = 0; i < work.size(); ++i)
		addCallees(work[i]);
	for (auto const& name: reachable)
	{
		result.reachableFunctions.insert(nameString(name));
		if (recursive.count(name))
			result.recursiveFunctions.insert(nameString(name));
	}

	// Propagate builtin requirements over solc's graph, including recursive
	// SCCs. The termination test is deliberately conservative: even a dead
	// return builtin retains its enclosing Solidity-frame lowering for now.
	auto const effects = SideEffectsPropagator::sideEffects(_dialect, graph);
	std::set<FunctionHandle> calldata, terminating;
	std::set<BuiltinHandle> calldataBuiltins;
	for (auto const* name: {"calldataload", "calldatacopy", "calldatasize"})
		if (auto handle = _dialect.findBuiltin(name))
			calldataBuiltins.insert(*handle);
	for (auto const& [caller, callees]: graph.functionCalls)
		for (auto const& callee: callees)
			if (auto const* builtin = std::get_if<BuiltinHandle>(&callee))
			{
				auto const& info = _dialect.builtin(*builtin);
				if (calldataBuiltins.count(*builtin))
					calldata.insert(caller);
				if (info.controlFlowSideEffects.canTerminate)
					terminating.insert(caller);
			}
	bool changed;
	do
	{
		changed = false;
		for (auto const& [caller, callees]: graph.functionCalls)
			for (auto const& callee: callees)
			{
				if (calldata.count(callee))
					changed |= calldata.insert(caller).second;
				if (terminating.count(callee))
					changed |= terminating.insert(caller).second;
			}
	} while (changed);
	for (auto const& name: reachable)
	{
		auto handle = FunctionHandle{name};
		if (calldata.count(handle))
			result.calldataFunctions.insert(nameString(name));
		if (terminating.count(handle))
			result.terminatingFunctions.insert(nameString(name));
		if (effects.at(handle).memory == SideEffects::Write)
			result.memoryWritingFunctions.insert(nameString(name));
	}

	// The side-effect propagator deliberately treats an EVM `call` as capable
	// of touching storage. That is correct for optimizer reordering, but it is
	// too conservative for our question: only an explicit sload/sstore needs a
	// concrete host contract and its storage dispatcher. In particular, using
	// the propagated flag internalized Solady's SafeTransferLib (which only
	// performs external calls) while leaving its library callers as root
	// subroutines, producing unresolved cross-scope calls.
	//
	// Ask the dialect for the actual storage builtin handles and inspect only
	// the root plus reachable local Yul functions. This remains independent of
	// builtin spelling and preserves transitive Yul-function reachability.
	auto const storageLoad = _dialect.storageLoadFunctionHandle();
	auto const storageStore = _dialect.storageStoreFunctionHandle();
	auto callsStorageBuiltin = [&](FunctionHandle const& caller) {
		auto const found = graph.functionCalls.find(caller);
		if (found == graph.functionCalls.end())
			return false;
		for (auto const& callee: found->second)
			if (auto const* builtin = std::get_if<BuiltinHandle>(&callee))
				if ((storageLoad && *builtin == *storageLoad)
					|| (storageStore && *builtin == *storageStore))
					return true;
		return false;
	};
	result.usesStorage = callsStorageBuiltin(FunctionHandle{YulName{}});
	if (!result.usesStorage)
		for (auto const& name: reachable)
			if (callsStorageBuiltin(FunctionHandle{name}))
			{
				result.usesStorage = true;
				break;
			}
	return result;
}

std::shared_ptr<PreparedAssembly const> SolcFacts::prepareAssembly(
	solidity::frontend::InlineAssembly const& _assembly)
{
	using Info = solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo;
	std::set<YulName> reserved;
	std::map<YulName, Info> externalByName;
	for (auto const& [identifier, info]: _assembly.annotation().externalReferences)
	{
		reserved.insert(identifier->name);
		externalByName.emplace(identifier->name, info);
	}
	auto result = std::make_shared<PreparedAssembly>();
	result->dialect = &_assembly.dialect();
	Disambiguator disambiguator(
		_assembly.dialect(), *_assembly.annotation().analysisInfo, reserved);
	result->block = disambiguator.translate(_assembly.operations().root());

	// External names are reserved, so every such name still denotes its solc
	// declaration. Re-key metadata with the COPIED identifiers, not stale pointers.
	struct Remap: ASTWalker
	{
		std::map<YulName, Info> const& byName;
		PreparedAssembly& out;
		Remap(std::map<YulName, Info> const& names, PreparedAssembly& assembly)
			: byName(names), out(assembly) {}
		void operator()(Identifier const& identifier) override
		{
			if (auto found = byName.find(identifier.name); found != byName.end())
				out.externalReferences.emplace(&identifier, found->second);
		}
		void operator()(Assignment const& assignment) override
		{
			ASTWalker::operator()(assignment);
			for (auto const& identifier: assignment.variableNames)
				if (auto found = out.externalReferences.find(&identifier);
					found != out.externalReferences.end()
					&& found->second.suffix == "slot" && found->second.declaration)
					out.assignedSlotDeclarations.insert(found->second.declaration->id());
		}
	};
	Remap remap(externalByName, *result);
	static_cast<ASTWalker&>(remap)(result->block);
	result->facts = analyzeYul(result->block, _assembly.dialect());
	for (auto const& [_, reference]: result->externalReferences)
		result->facts.usesStorage |= reference.suffix == "slot";
	return result;
}

SolcFacts::YulArgumentFacts SolcFacts::yulArgumentFacts(
	PreparedAssembly const& _assembly,
	std::map<std::string, std::string> const& _externalConstants)
{
	YulArgumentFacts result;
	auto const& facts = _assembly.facts;
	if (facts.reachableFunctions.empty() || !_assembly.dialect)
		return result;
	auto const& dialect = *_assembly.dialect;
	std::map<YulName, std::vector<Expression const*>> incoming;
	struct Calls: ASTWalker
	{
		YulAnalysis const& facts;
		decltype(incoming)& args;
		Calls(YulAnalysis const& f, decltype(incoming)& a): facts(f), args(a) {}
		void operator()(FunctionDefinition const& f) override
		{
			if (facts.reachableFunctions.count(f.name.str()))
				ASTWalker::operator()(f);
		}
		void operator()(FunctionCall const& call) override
		{
			if (auto const* id = std::get_if<Identifier>(&call.functionName))
				if (auto f = facts.functions.find(id->name.str()); f != facts.functions.end())
					for (size_t i = 0; i < call.arguments.size(); ++i)
					{
						auto const& p = f->second->parameters.at(i).name;
						if (!facts.assignedVariables.count(p.str()))
							args[p].push_back(&call.arguments[i]);
					}
			ASTWalker::operator()(call);
		}
	};
	Calls calls(facts, incoming);
	static_cast<ASTWalker&>(calls)(_assembly.block);

	SSAValueTracker ssa;
	ssa(_assembly.block);
	std::map<YulName, AssignedValue> values;
	for (auto const& [name, value]: ssa.values())
	{
		if (!value || !SideEffectsCollector(dialect, *value).movable())
			continue;
		// An immutable local can snapshot a MUTABLE variable. Its initializer
		// must not be reinterpreted using that variable's later value.
		auto refs = VariableReferencesCounter::countReferences(*value);
		if (std::none_of(refs.begin(), refs.end(), [&](auto const& ref) {
			return facts.assignedVariables.count(ref.first.str());
		}))
			values.emplace(name, AssignedValue{value, 0});
	}
	std::map<YulName, Expression> literals;
	auto bindConstant = [&](YulName name, std::string const& value) {
		auto [it, inserted] = literals.emplace(name,
			Literal{{}, LiteralKind::Number, LiteralValue(solidity::u256{value})});
		values[name] = AssignedValue{&it->second, 0};
	};
	for (auto const& [name, value]: _externalConstants)
		if (!facts.assignedVariables.count(name) && !value.empty()
			&& value.find_first_not_of("0123456789") == std::string::npos)
			bindConstant(YulName{name}, value);

	bool changed;
	do
	{
		changed = false;
		// A fresh immutable snapshot per iteration: the knowledge base must
		// not retain relations across newly discovered parameter constants.
		auto snapshot = values;
		KnowledgeBase knowledge(snapshot, dialect);
		std::set<YulName> active;
		std::function<std::optional<unsigned>(Expression const&)> residue =
			[&](Expression const& expr) -> std::optional<unsigned> {
			if (auto constant = knowledge.valueIfKnownConstant(expr))
				return static_cast<unsigned>(*constant & 31);
			if (auto const* id = std::get_if<Identifier>(&expr))
			{
				if (auto r = result.residuesMod32.find(id->name.str()); r != result.residuesMod32.end())
					return r->second;
				auto it = snapshot.find(id->name);
				if (it == snapshot.end() || !active.insert(id->name).second)
					return std::nullopt;
				auto r = residue(*it->second.value);
				active.erase(id->name);
				return r;
			}
			auto const* call = std::get_if<FunctionCall>(&expr);
			auto const* builtin = call ? std::get_if<BuiltinName>(&call->functionName) : nullptr;
			if (!builtin)
				return std::nullopt;
			auto const& op = dialect.builtin(builtin->handle).name;
			auto const& args = call->arguments;
			if (op == "not" && args.size() == 1)
			{
				auto r = residue(args[0]);
				return r ? std::optional<unsigned>(31 ^ *r) : std::nullopt;
			}
			if (args.size() != 2)
				return std::nullopt;
			auto l = residue(args[0]), r = residue(args[1]);
			if (op == "add" && l && r) return (*l + *r) % 32;
			if (op == "sub" && l && r) return (*l + 32 - *r) % 32;
			if (op == "mul" && l && r) return (*l * *r) % 32;
			if ((op == "mul" || op == "and") && (l == 0 || r == 0)) return 0;
			if (op == "and" && l && r) return *l & *r;
			if (op == "or" && l && r) return *l | *r;
			if (op == "xor" && l && r) return *l ^ *r;
			if (op == "shl")
				if (auto shift = knowledge.valueIfKnownConstant(args[0]))
				{
					if (*shift >= 5) return 0;
					if (r) return (*r << static_cast<unsigned>(*shift)) % 32;
				}
			return std::nullopt;
		};
		for (auto const& [parameter, args]: incoming)
		{
			auto constant = knowledge.valueIfKnownConstant(*args.front());
			auto alignment = residue(*args.front());
			for (auto const* arg: args)
			{
				if (constant != knowledge.valueIfKnownConstant(*arg)) constant.reset();
				if (alignment != residue(*arg)) alignment.reset();
			}
			if (constant && result.constants.emplace(parameter.str(), constant->str()).second)
			{
				bindConstant(parameter, constant->str());
				changed = true;
			}
			if (alignment)
				changed |= result.residuesMod32.emplace(parameter.str(), *alignment).second;
		}
	} while (changed);
	return result;
}

std::vector<uint8_t> SolcFacts::externalSelector(
	solidity::frontend::FunctionType const& _function)
{
	auto bytes = solidity::toBigEndian(_function.externalIdentifier());
	return {bytes.end() - 4, bytes.end()};
}

std::vector<uint8_t> SolcFacts::externalSelector(std::string const& _signature)
{
	auto hash = solidity::util::keccak256(_signature).asBytes();
	return {hash.begin(), hash.begin() + 4};
}

std::vector<uint8_t> SolcFacts::signatureHash(std::string const& _signature)
{
	return solidity::util::keccak256(_signature).asBytes();
}

std::vector<uint8_t> SolcFacts::interfaceId(
	solidity::frontend::ContractDefinition const& _contract)
{
	auto id = _contract.interfaceId();
	return {
		static_cast<uint8_t>((id >> 24) & 0xff),
		static_cast<uint8_t>((id >> 16) & 0xff),
		static_cast<uint8_t>((id >> 8) & 0xff),
		static_cast<uint8_t>(id & 0xff),
	};
}

} // namespace puyasol::builder
