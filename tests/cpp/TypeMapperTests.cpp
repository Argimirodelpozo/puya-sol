#include "builder/types/TypeMapper.h"
#include "builder/context/BuildArtifacts.h"
#include "builder/context/CompilationSession.h"
#include "builder/contract/AWSTBuilder.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/solc/SourceLocConvert.h"
#include "builder/types/EncodedSize.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/codec/Arc4ArrayWidening.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/ConversionPlan.h"
#include "awst/TupleValue.h"
#include "builder/solc/StorageRefPointer.h"
#include "builder/codec/SlotWordCodec.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>
#include <libsolidity/interface/CompilerStack.h>

#include <iostream>
#include <stdexcept>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<puyasol::builder::CompilationSession>);
static_assert(!std::is_move_constructible_v<puyasol::builder::CompilationSession>);
static_assert(!std::is_copy_assignable_v<puyasol::builder::CompilationSession>);
static_assert(!std::is_move_assignable_v<puyasol::builder::CompilationSession>);
static_assert(!std::is_move_constructible_v<puyasol::builder::AWSTBuilder>);

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
	bool unscopedRejected = false;
	try { artifacts.contract(); }
	catch (std::logic_error const&) { unscopedRejected = true; }
	require(unscopedRejected, "unscoped contract helper use was accepted");
	{
		builder::BuildArtifacts::ContractScope outer(artifacts);
		artifacts.contract().helpers["host"] = "outer";
		{
			builder::BuildArtifacts::ContractScope inner(artifacts);
			require(artifacts.contract().helpers.empty(), "nested contract inherited host helpers");
			artifacts.contract().helpers["host"] = "inner";
		}
		require(artifacts.contract().helpers.at("host") == "outer", "nested build lost its host emission state");
		try
		{
			builder::BuildArtifacts::ContractScope inner(artifacts);
			throw std::runtime_error("nested build failed");
		}
		catch (std::runtime_error const&) {}
		require(artifacts.contract().helpers.at("host") == "outer", "failed build leaked its emission scope");
	}
	artifacts.clear();
	builder::TypeMapper mapper(analysis, _profile, sources, artifacts);
	auto const* bytes32 = TypeProvider::fixedBytes(32);
	auto const* array32 = TypeProvider::array(DataLocation::Storage, TypeProvider::uint(8), 32);
	require(builder::SlotWordCodec::supportsField(mapper.mapSolTypeToARC4(bytes32), bytes32, 32),
		"scalar codec rejected bytes32");
	require(!builder::SlotWordCodec::supportsField(mapper.mapSolTypeToARC4(array32), array32, 32),
		"equal-size uint8[32] aggregate was mistaken for a scalar word");
	require(!builder::SlotWordCodec::supportsField(awst::WType::uint64Type(), TypeProvider::uint(16), 1),
		"scalar codec ignored solc's storage width");
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
	auto const* sourceA = dynamic_cast<StructType const*>(mapper.solcAggregateFor(mappedA));
	require(sourceA && &sourceA->structDefinition() == &structA->structDefinition(),
		"mapped struct lost its canonical solc member facts");
	require(!mapper.solcAggregateFor(awst::WType::bytesType()),
		"bytes placeholder acquired a guessed aggregate identity");
	auto const* wrapper = TypeProvider::structType(
		declaration<StructDefinition>(a, "Wrapper"), DataLocation::Storage);
	auto const* inner = builder::transparentMappingWrapper(wrapper);
	require(inner && !builder::transparentMappingWrapper(structA),
		"transparent wrapper classification ignored solc storage-only shape");
	auto const* wrapperW = dynamic_cast<awst::ARC4Struct const*>(mapper.map(wrapper));
	auto const* innerW = dynamic_cast<awst::ARC4Struct const*>(mapper.map(inner));
	require(wrapperW && innerW && wrapperW != innerW && wrapperW->fields() == innerW->fields(),
		"transparent wrapper lost nominal identity or changed the inner representation");
	require(mapper.solcAggregateFor(wrapperW) == mapper.solcAggregateFor(innerW),
		"transparent wrapper did not retain its represented member facts");
	{
		auto slotProfile = _profile;
		slotProfile.evmStorageLayout = true;
		builder::TypeMapper slotMapper(analysis, slotProfile, sources, artifacts);
		auto const* slotWrapper = dynamic_cast<awst::ARC4Struct const*>(slotMapper.map(wrapper));
		require(slotWrapper && slotWrapper->fields().size() == 1
			&& slotWrapper->fields().front().first == "inner",
			"default wrapper normalization changed slot-mode representation");
	}

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
	builder::TypeCoercion::assertImplicitlyConvertible(calldataFunction, memoryFunction, {}, "callable view test");
	require(!Logger::instance().hasErrors(), "solc's external callable view was rejected");

	// Calldata slices retain the underlying array's element representation,
	// without calling the slice type's forbidden copyForLocation().
	auto const* calldataArray = TypeProvider::array(DataLocation::CallData, TypeProvider::uint256());
	require(awst::structurallyEquivalent(mapper.map(TypeProvider::arraySlice(*calldataArray)),
		mapper.map(calldataArray)), "calldata slice lost its array element representation");

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
	auto const* recursiveWrapper = TypeProvider::structType(
		declaration<StructDefinition>(a, "RecursiveWrapper"), DataLocation::Storage);
	require(!builder::transparentMappingWrapper(recursiveWrapper),
		"recursive wrapper was treated as a transparent finite value");
	require(builder::hasDynamicStorageShape(recursive), "recursive array shape was lost");
	auto const* fixedCallback = TypeProvider::structType(
		declaration<StructDefinition>(a, "FixedCallback"), DataLocation::Storage);
	auto const* dynamicCallback = TypeProvider::structType(
		declaration<StructDefinition>(a, "DynamicCallback"), DataLocation::Storage);
	require(!builder::hasDynamicStorageShape(fixedCallback), "a fixed callback is not dynamic");
	require(builder::hasDynamicStorageShape(dynamicCallback), "dynamic callback array was treated as fixed");
	require(builder::hasDynamicStorageShape(TypeProvider::array(DataLocation::Storage, dynamicCallback, 2)),
		"a fixed container hid its dynamic element");
	require(!builder::hasDynamicStorageShape(TypeProvider::array(DataLocation::Storage, fixedCallback, 2)),
		"a fixed callback array was treated as dynamic");
	require(mapper.isBoxKeyedStorageRef(dynamicCallback), "dynamic struct handle disagrees with placement");
	require(!mapper.isBoxKeyedStorageRef(fixedCallback), "small fixed callback struct was forced to a box");
	auto const* packed = TypeProvider::structType(
		declaration<StructDefinition>(a, "Packed"), DataLocation::Storage);
	auto const* alwaysBoxed = TypeProvider::structType(
		declaration<StructDefinition>(a, "AlwaysBoxed"), DataLocation::Storage);
	require(packed->storageSizeUpperBound() >= 4
		&& builder::computeEncodedElementSize(mapper.map(packed)).fixedBytes() == 3,
		"fixture no longer distinguishes EVM upper bounds from actual AVM encoding");
	require(!mapper.isBoxKeyedStorageRef(packed), "compact global struct was passed as a box key");
	require(mapper.isBoxKeyedStorageRef(alwaysBoxed), "128-byte struct lost its required box key");
	for (auto* facts: {&analysis.refPassedStructs, &analysis.boxKeyedStructs})
	{
		facts->insert(packed->structDefinition().id());
		require(mapper.isBoxKeyedStorageRef(packed), "source reference facts lost their required box key");
		facts->erase(packed->structDefinition().id());
	}
	for (int reset = 0; reset < 2; ++reset)
	{
		auto const* mapped = dynamic_cast<awst::ARC4Struct const*>(mapper.mapSolTypeToARC4(recursive));
		require(mapper.mapSolTypeToARC4(recursive) == mapped && mapper.map(recursive) == mapped,
			"a provisional projection poisoned the full ARC4 type cache");
		auto const* array = dynamic_cast<awst::ARC4DynamicArray const*>(mapper.map(
			TypeProvider::array(DataLocation::Storage, recursive)));
		require(array && array->elementType() == mapped, "standalone recursive array lost its full element type");
		require(mapped && mapped->fields().size() == 2, "recursive root lost fields");
		auto const* children = dynamic_cast<awst::ARC4DynamicArray const*>(mapped->fields()[1].second);
		auto const* projection = children
			? dynamic_cast<awst::ARC4Struct const*>(children->elementType()) : nullptr;
		require(projection && projection->fields().size() == 2
			&& projection->fields()[1].second == awst::WType::bytesType(),
			"recursive projection is not finite and field-preserving");
		require(mapper.solcAggregateFor(projection) && mapper.solcAggregateFor(children),
			"recursive alias projection lost solc aggregate facts");
		require(mapper.map(TypeProvider::withLocationIfReference(DataLocation::Memory, recursive)) == mapped,
			"recursive root has a location-dependent projection");
		auto retainedRoot = std::make_shared<awst::Subroutine>();
		retainedRoot->typeArena = mapper.typeArena();
		retainedRoot->returnType = mapped;
		std::weak_ptr<awst::WTypeArena const> arena = mapper.typeArena();
		mapper.reset();
		require(!arena.expired() && mapped->fields().size() == 2,
			"returned root lost its WTypes after mapper reset");
		require(!mapper.solcAggregateFor(mapped), "reset retained stale solc aggregate facts");
		retainedRoot.reset();
		require(arena.expired(), "type arena leaked after its last root was released");
	}
	std::vector<std::string> shapes;
	for (bool reverse: {false, true})
	{
		mapper.reset();
		auto const* left = TypeProvider::structType(declaration<StructDefinition>(a, "Left"), DataLocation::Storage);
		auto const* right = TypeProvider::structType(declaration<StructDefinition>(a, "Right"), DataLocation::Storage);
		mapper.map(reverse ? right : left);
		std::vector<std::string> current;
		for (auto const* root: {left, right})
		{
			auto const* full = mapper.mapSolTypeToARC4(root);
			require(full == mapper.map(root) && full == mapper.mapSolTypeToARC4(root),
				"mutually recursive full types are unstable");
			require(full != mapper.mapStruct(root, true), "projection aliased a full value");
			current.push_back(builder::TypeCoercion::wtypeToABIName(full));
		}
		if (reverse) require(shapes == current, "recursive shape depends on mapping order");
		else shapes = current;
	}
}

void testValueAdapters()
{
	using namespace puyasol;
	using namespace builder;
	awst::SourceLocation loc;
	awst::ARC4UIntN u8(8), u16(16), i8(8, "int8"), i128(128, "int128");
	awst::ARC4DynamicArray dynamic(&u16);
	awst::ARC4StaticArray pair(&dynamic, 2), huge(&dynamic, 10000);
	awst::ARC4Tuple unsupportedTuple({awst::WType::voidType()});
	awst::ARC4DynamicArray unsupportedArray(awst::WType::voidType());
	auto encoded = std::dynamic_pointer_cast<awst::BytesConstant>(TypeCoercion::makeDefaultValue(&pair, loc));
	require(encoded && encoded->value == std::vector<uint8_t>({0, 4, 0, 6, 0, 0, 0, 0}),
		"default dynamic offsets disagreed with the size classifier");
	for (auto const* unsupported: std::vector<awst::WType const*>{&huge, &unsupportedTuple, &unsupportedArray})
	{
		bool rejected = false;
		try { TypeCoercion::makeDefaultValue(unsupported, loc); }
		catch (SizeError const&) { rejected = true; }
		require(rejected, "failed ARC4 default became an empty value");
	}
	awst::ARC4Tuple signedFields({&i8, &i128});
	require(TypeCoercion::wtypeToABIName(&signedFields) == "(int8,int128)", "signed wire aliases were erased");
	require(TypeCoercion::wtypeToABIName(awst::WType::biguintType()) == "uint512", "native Puya wire width was guessed");
	awst::ARC4StaticArray fixed(&u8, 2), widened(&u16, 4), loopSource(&u8, 257), loopTarget(&u16, 259);
	std::vector<std::shared_ptr<awst::Statement>> effects;
	require(bool(tryConvertArc4Array(awst::makeVarExpression("a", &fixed, loc), &widened, &effects, loc))
		&& effects.empty(), "small copy lost its expression-local strategy");
	require(!tryConvertArc4Array(awst::makeVarExpression("a", &loopSource, loc), &loopTarget, nullptr, loc),
		"loop copy claimed a conversion without a statement sink");
	require(bool(tryConvertArc4Array(awst::makeVarExpression("a", &loopSource, loc), &loopTarget, &effects, loc))
		&& !effects.empty(), "large integer copy did not use its loop strategy");
	awst::ARC4DynamicArray bools(awst::WType::arc4BoolType());
	awst::ARC4StaticArray fixedBools(awst::WType::arc4BoolType(), 9);
	auto packed = tryConvertArc4Array(awst::makeVarExpression("b", &fixedBools, loc), &bools, nullptr, loc);
	require(packed && packed->nodeType() == "ConvertArray", "packed bools were treated as byte-strided elements");
	awst::ARC4StaticArray singleDynamic(&dynamic, 1);
	auto nested = std::dynamic_pointer_cast<awst::NewArray>(tryConvertArc4Array(
		awst::makeVarExpression("nested", &singleDynamic, loc), &pair, nullptr, loc));
	require(nested && nested->values.size() == 2 && nested->values[1]->wtype == &dynamic,
		"fixed copies of dynamic elements did not rebuild offsets and default tails");

	awst::WTuple sourceW({awst::WType::uint64Type(), awst::WType::uint64Type()});
	awst::WTuple targetW({awst::WType::biguintType(), awst::WType::biguintType()});
	auto literal = awst::makeTupleExpression(&sourceW, loc);
	literal->items = {awst::makeOne(loc), awst::makeOne(loc)};
	auto const* source = TypeProvider::tuple(TypePointers{TypeProvider::integer(8, IntegerType::Modifier::Signed), TypeProvider::integer(8, IntegerType::Modifier::Signed)});
	auto const* target = TypeProvider::tuple(TypePointers{TypeProvider::integer(128, IntegerType::Modifier::Signed), TypeProvider::integer(128, IntegerType::Modifier::Signed)});
	auto converted = ConversionPlan{source, target, &targetW, ConversionPlan::Context::Return}.emit(literal, loc);
	require(converted != literal && literal->wtype == &sourceW && literal->items[0]->wtype == awst::WType::uint64Type(),
		"tuple adaptation mutated the shared source AST");
	effects.clear();
	auto items = awst::tupleItems(literal, loc, &effects);
	require(items.size() == 2 && effects.size() == 1, "tuple snapshot did not evaluate the complete source once");
	awst::BytesWType bytes5(5);
	auto padded = std::dynamic_pointer_cast<awst::ReinterpretCast>(
		ConversionPlan{TypeProvider::stringLiteral("hi"), TypeProvider::fixedBytes(5),
			&bytes5, ConversionPlan::Context::Return}.emit(
				awst::makeVarExpression("literal_snapshot", awst::WType::stringType(), loc), loc));
	auto concat = padded ? std::dynamic_pointer_cast<awst::IntrinsicCall>(padded->expr) : nullptr;
	require(concat && concat->opCode == "concat", "literal width was lost behind a tuple snapshot");
	for (int width: {1, 4, 8, 16, 32})
	{
		awst::BytesWType bytes(width);
		for (auto text: {"0", "255", "256", "0x1234"})
		{
			auto input = awst::makeIntegerConstant(text, loc, awst::WType::biguintType());
			auto result = std::dynamic_pointer_cast<awst::BytesConstant>(TypeCoercion::coerceScalar(input, &bytes, loc));
			require(result && result->value == TypeCoercion::intLiteralToBytesN(text, width)
				&& input->wtype == awst::WType::biguintType(), "scalar integer-to-bytes fast path lost width or mutated its source");
		}
	}
	auto byteLiteral = awst::makeBytesConstant({0xab, 0xcd}, loc);
	auto fixedLiteral = std::dynamic_pointer_cast<awst::BytesConstant>(TypeCoercion::coerceScalar(byteLiteral, &bytes5, loc));
	require(fixedLiteral && fixedLiteral->value == std::vector<uint8_t>({0xab, 0xcd, 0, 0, 0})
		&& byteLiteral->value.size() == 2, "byte literal padding changed direction or mutated the input");
	for (auto const& input: {std::vector<uint8_t>{}, std::vector<uint8_t>{1, 2, 3, 4, 5, 6}})
	{
		auto expected = input;
		expected.resize(5, 0);
		auto converted = std::dynamic_pointer_cast<awst::BytesConstant>(TypeCoercion::coerceScalar(
			awst::makeBytesConstant(input, loc), &bytes5, loc));
		require(converted && converted->value == expected && converted->wtype == &bytes5,
			"fixed bytes conversion must preserve the prefix and pad empty values");
	}
}
}

int main(int argc, char** argv)
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
struct Packed { uint8 a; uint8 b; uint8 c; }
struct AlwaysBoxed { uint256[4] words; }
struct FixedCallback { uint16 tag; function() internal returns (uint256) callback; }
struct DynamicCallback { function() internal returns (uint256)[] callbacks; }
struct Holder { uint256[] values; mapping(uint256 => uint256) entries; }
struct Wrapper { Holder inner; }
struct RecursiveHolder { RecursiveWrapper[] children; mapping(uint256 => uint256) entries; }
struct RecursiveWrapper { RecursiveHolder inner; }
struct Left { uint16 value; Right[] children; }
struct Right { bool flag; Left parent; }
)"},
			{"b.sol", R"(pragma solidity ^0.8.20;
struct Item { uint16 first; bool second; }
enum Choice { One, Two }
type Value is uint16;
contract Target {}
)"},
		});
		require(compiler.parseAndAnalyze(), "solc rejected the type identity fixture");
		if (argc > 1)
		{
			using namespace puyasol;
			auto const* source = TypeProvider::uint256();
			Type const* target = nullptr;
			if (std::string(argv[1]) == "udvt")
			{
				target = TypeProvider::userDefinedValueType(
					declaration<UserDefinedValueTypeDefinition>(compiler.ast("a.sol"), "Value"));
				source = TypeProvider::uint(16);
				builder::TypeCoercion::assertImplicitlyConvertible(source, target, {}, "UDVT rejection test");
			}
			else
			{
				auto const* f = TypeProvider::function(TypePointers{source}, TypePointers{}, {"p"}, {}, FunctionType::Kind::External);
				auto const* g = TypeProvider::function(TypePointers{TypeProvider::boolean()}, TypePointers{}, {"p"}, {}, FunctionType::Kind::External);
				builder::TypeCoercion::assertImplicitlyConvertible(f, g, {}, "function rejection test");
			}
			require(Logger::instance().hasErrors(), "an illegal nominal/function conversion bypassed solc");
			return 0;
		}
		testValueAdapters();
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
