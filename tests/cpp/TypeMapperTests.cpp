#include "builder/sol-types/TypeMapper.h"
#include "builder/BuildArtifacts.h"
#include "builder/ProgramAnalysis.h"
#include "builder/SourceLocConvert.h"
#include "builder/sol-types/EncodedSize.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>
#include <libsolidity/interface/CompilerStack.h>

#include <iostream>
#include <stdexcept>

namespace
{
using namespace solidity::frontend;

template <class T>
T const& declaration(SourceUnit const& _source, std::string const& _name)
{
	for (auto const& node: _source.nodes())
		if (auto const* decl = dynamic_cast<T const*>(node.get()); decl && decl->name() == _name)
			return *decl;
	throw std::runtime_error("missing test declaration " + _name);
}

void require(bool _condition, char const* _message)
{
	if (!_condition) throw std::runtime_error(_message);
}

void testMapper(CompilerStack const& _compiler, puyasol::builder::TargetProfile const& _profile)
{
	using namespace puyasol;
	builder::ProgramAnalysis analysis;
	builder::SourceMap sources;
	builder::BuildArtifacts artifacts;
	builder::TypeMapper mapper(analysis, _profile, sources, artifacts);
	auto const& a = _compiler.ast("a.sol");
	auto const& b = _compiler.ast("b.sol");
	auto const* structA = TypeProvider::structType(
		declaration<StructDefinition>(a, "Item"), DataLocation::Storage);
	auto const* structB = TypeProvider::structType(
		declaration<StructDefinition>(b, "Item"), DataLocation::Storage);
	auto const* memA = TypeProvider::withLocationIfReference(DataLocation::Memory, structA);
	auto const* memB = TypeProvider::withLocationIfReference(DataLocation::Memory, structB);
	std::vector<std::pair<Type const*, Type const*>> nominalTypes{
		{structA, structB},
		{TypeProvider::enumType(declaration<EnumDefinition>(a, "Choice")),
			TypeProvider::enumType(declaration<EnumDefinition>(b, "Choice"))},
		{TypeProvider::userDefinedValueType(declaration<UserDefinedValueTypeDefinition>(a, "Value")),
			TypeProvider::userDefinedValueType(declaration<UserDefinedValueTypeDefinition>(b, "Value"))},
		{TypeProvider::contract(declaration<ContractDefinition>(a, "Target")),
			TypeProvider::contract(declaration<ContractDefinition>(b, "Target"))},
		{TypeProvider::function(TypePointers{memA}, TypePointers{}, {"p"}, {}),
			TypeProvider::function(TypePointers{memB}, TypePointers{}, {"p"}, {})},
		{TypeProvider::function(TypePointers{memA}, TypePointers{}, {"p"}, {}, FunctionType::Kind::External),
			TypeProvider::function(TypePointers{memB}, TypePointers{}, {"p"}, {}, FunctionType::Kind::External)},
	};
	for (auto const& [left, right]: nominalTypes)
	{
		require(left->identifier() != right->identifier(), "solc nominal identities unexpectedly alias");
		auto const* arrayA = TypeProvider::array(DataLocation::Storage, left, 2);
		auto const* arrayB = TypeProvider::array(DataLocation::Storage, right, 2);
		auto const* mapped = mapper.map(arrayA);
		require(mapped != mapper.map(arrayB), "same-named nested declarations aliased in the cache");
		for (auto location: {DataLocation::Memory, DataLocation::CallData, DataLocation::Storage})
			for (bool pointer: {false, true})
			{
				auto const* located = TypeProvider::withLocationIfReference(location, arrayA, pointer);
				require(mapper.map(located) == mapped, "array value locations do not intern together");
				auto const* tuple = TypeProvider::tuple(TypePointers{located, nullptr, located});
				auto const* canonicalTuple = TypeProvider::tuple(TypePointers{arrayA, nullptr, arrayA});
				require(mapper.map(tuple) == mapper.map(canonicalTuple), "tuple value locations do not intern together");
			}
		// Fresh solc objects with the same identifier must also share the WType.
		require(mapper.map(TypeProvider::array(DataLocation::Storage, left, 2)) == mapped,
			"equivalent fresh solc array types did not intern");
	}

	auto const* mappedA = dynamic_cast<awst::ARC4Struct const*>(mapper.map(structA));
	auto const* mappedB = dynamic_cast<awst::ARC4Struct const*>(mapper.map(structB));
	require(mappedA && mappedB && mappedA != mappedB
		&& mappedA->fields().size() == 1 && mappedB->fields().size() == 2,
		"nominal struct field layouts were lost");
	require(mapper.map(memA) == mappedA, "struct location normalization changed identity");

	// Callable locations are semantic signature facts, not buffer locations.
	auto const* calldataA = TypeProvider::withLocationIfReference(DataLocation::CallData, structA);
	auto const* memoryFunction = TypeProvider::function(
		TypePointers{memA}, TypePointers{}, {"p"}, {}, FunctionType::Kind::External);
	auto const* calldataFunction = TypeProvider::function(
		TypePointers{calldataA}, TypePointers{}, {"p"}, {}, FunctionType::Kind::External);
	require(mapper.map(memoryFunction) != mapper.map(calldataFunction),
		"callable parameter locations were erased");
	require(awst::structurallyEquivalent(mapper.map(memoryFunction), mapper.map(calldataFunction)),
		"equal-width callable handles acquired different physical encodings");

	// Calldata slices are reference types, but cannot be relocated by solc.
	auto const* calldataArray = TypeProvider::array(DataLocation::CallData, TypeProvider::uint256());
	require(mapper.map(TypeProvider::arraySlice(*calldataArray)) == awst::WType::bytesType(),
		"calldata slice mapping attempted value-buffer relocation");

	// Layout-only mapping must not poison the recursive cache when a nested
	// value is too large. Actual materialization must still fail on every try.
	auto const* hugeStruct = TypeProvider::structType(
		declaration<StructDefinition>(a, "Huge"), DataLocation::Storage);
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		require(!mapper.tryMapStorageRepresentation(hugeStruct),
			"oversized storage value acquired a guessed representation");
		bool rejected = false;
		try { mapper.map(hugeStruct); }
		catch (builder::SizeError const&) { rejected = true; }
		require(rejected, "layout-only mapping suppressed a later materialization error");
	}

	auto const* recursive = TypeProvider::structType(
		declaration<StructDefinition>(a, "Node"), DataLocation::Storage);
	for (int reset = 0; reset < 2; ++reset)
	{
		auto const* mapped = dynamic_cast<awst::ARC4Struct const*>(mapper.map(recursive));
		require(mapped && mapped->fields().size() == 2, "recursive root lost fields");
		auto const* children = dynamic_cast<awst::ARC4DynamicArray const*>(mapped->fields()[1].second);
		auto const* projection = children
			? dynamic_cast<awst::ARC4Struct const*>(children->elementType()) : nullptr;
		require(projection && projection->fields().size() == 2
			&& projection->fields()[1].second == awst::WType::bytesType(),
			"recursive projection is not finite and field-preserving");
		require(mapper.map(TypeProvider::withLocationIfReference(DataLocation::Memory, recursive)) == mapped,
			"recursive root has a location-dependent projection");
		mapper.reset();
	}
}
}

int main()
{
	try
	{
		solidity::frontend::CompilerStack compiler;
		compiler.setSources({
			{"a.sol", R"(pragma solidity ^0.8.20;
struct Item { uint16 first; }
enum Choice { One, Two }
type Value is uint16;
contract Target {}
struct Node { uint16 value; Node[] children; }
struct Huge { uint256[134217728] values; }
)"},
			{"b.sol", R"(pragma solidity ^0.8.20;
struct Item { uint16 first; bool second; }
enum Choice { One, Two }
type Value is uint16;
contract Target {}
)"},
		});
		require(compiler.parseAndAnalyze(), "solc rejected the type identity fixture");
		for (auto abi: {puyasol::builder::ContractAbi::Arc4, puyasol::builder::ContractAbi::Evm})
		{
			puyasol::builder::TargetProfile profile;
			profile.contractAbi = abi;
			testMapper(compiler, profile);
		}
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
