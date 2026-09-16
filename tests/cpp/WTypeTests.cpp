#include "awst/WType.h"
#include "builder/storage/StoragePlace.hpp"
#include "builder/target/EvmFeaturePolicy.h"

#include <iostream>
#include <string>

namespace
{

bool require(bool _condition, std::string const& _message)
{
	if (_condition)
		return true;
	std::cerr << _message << '\n';
	return false;
}

} // namespace

int main()
{
	using namespace puyasol::awst;
	ARC4UIntN uint128a(128);
	ARC4UIntN uint128b(128);
	ARC4UIntN uint256(256);
	ARC4StaticArray arrayA(&uint128a, 4);
	ARC4StaticArray arrayB(&uint128b, 4);
	ARC4StaticArray differentLength(&uint128b, 5);
	ARC4StaticArray differentElement(&uint256, 4);
	ARC4Struct structA("S", {{"values", &arrayA}}, false);
	ARC4Struct structB("S", {{"values", &arrayB}}, false);
	ARC4Struct differentField("S", {{"other", &arrayB}}, false);

	bool ok = true;
	WTuple mutableTuple({&arrayA}), immutableTuple({&uint128a}), emptyTuple({});
	ARC4Tuple mutableArc4Tuple({&arrayA}), immutableArc4Tuple({&uint128a}), emptyArc4Tuple({});
	ARC4Struct frozenMutable("Frozen", {{"values", &mutableArc4Tuple}}, true);
	ARC4Struct frozenImmutable("Frozen", {{"value", &immutableArc4Tuple}}, true);
	ok &= require(!mutableTuple.immutable() && !mutableArc4Tuple.immutable()
		&& !frozenMutable.immutable(), "tuple/struct ignored mutable descendants");
	ok &= require(immutableTuple.immutable() && immutableArc4Tuple.immutable()
		&& emptyTuple.immutable() && emptyArc4Tuple.immutable() && frozenImmutable.immutable(),
		"immutable tuple/struct differs from Puya's element-derived rule");
	NameGen::resetAll();
	int first = nextSingleEvalId();
	{
		NameGen::Scope nested;
		ok &= require(nextSingleEvalId() == first, "nested naming context did not start deterministically");
	}
	ok &= require(nextSingleEvalId() == first + 1, "nested naming context was not restored");
	bool invalidCoinbase = false;
	try { puyasol::builder::decodeEvmCoinbase20("bad"); }
	catch (std::invalid_argument const&) { invalidCoinbase = true; }
	ok &= require(invalidCoinbase, "invalid programmatic coinbase silently became zero");
	ok &= require(&arrayA != &arrayB && structurallyEquivalent(&arrayA, &arrayB),
		"equivalent non-interned arrays were not recognized");
	ok &= require(structurallyEquivalent(&structA, &structB),
		"equivalent nested structs were not recognized");
	ok &= require(!structurallyEquivalent(&arrayA, &differentLength),
		"array length mismatch was ignored");
	ok &= require(!structurallyEquivalent(&arrayA, &differentElement),
		"array element mismatch was ignored");
	ok &= require(!structurallyEquivalent(&structA, &differentField),
		"struct field mismatch was ignored");
	ok &= require(!structurallyEquivalent(&arrayA, nullptr),
		"null type was treated as equivalent");

	// Box lifecycle facts survive alias reconstruction independently of key shape.
	auto box = makeBoxValueExpression(makeUtf8BytesConstant("root", {}, WType::boxKeyType()),
		&arrayA, {});
	box->preserveEmptyBox = true;
	auto read = makeStateGet(box, makeBytesConstant({}, {}), &arrayA, {});
	auto wrapped = makeReinterpretCast(read, &arrayA, {});
	auto place = puyasol::builder::StoragePlace::fromRead(wrapped);
	ok &= require(place && place->preserveEmptyBox && place->valueType == &arrayA,
		"storage origin was lost through interleaved read/cast wrappers");
	if (place)
	{
		auto rebuilt = place->makeField(makeVarExpression("key", WType::bytesType(), {}), {});
		auto const* rebuiltBox = dynamic_cast<BoxValueExpression const*>(rebuilt.get());
		ok &= require(rebuiltBox && rebuiltBox->preserveEmptyBox,
			"storage alias reconstruction lost declaration origin");
	}
	box->preserveEmptyBox = false;
	place = puyasol::builder::StoragePlace::fromRead(box);
	ok &= require(place && !place->preserveEmptyBox,
		"a literal runtime key was mistaken for an initialized declaration");

	using puyasol::builder::StoragePlace;
	auto index = makeIndexExpression(box, makeIntegerConstant(0, {}), &uint128a, {});
	auto decoded = makeARC4Decode(index, WType::biguintType(), {});
	ok &= require(StoragePlace::projectionBase(decoded) == index
		&& StoragePlace::projectionBase(index) == box
		&& StoragePlace::projectionBase(read) == box,
		"storage projection traversal lost an address-preserving wrapper");
	ok &= require(!StoragePlace::fromRead(index)
		&& !StoragePlace::projectionBase(makeVarExpression("local", &arrayA, {})),
		"an interior projection or materialized local was mistaken for a root");

	return ok ? 0 : 1;
}
