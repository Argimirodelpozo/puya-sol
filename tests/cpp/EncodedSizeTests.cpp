#include "builder/sol-types/Arc4Defaults.h"

#include <iostream>
#include <limits>

int main()
{
	using namespace puyasol;
	using namespace builder;
	using Kind = EncodedSize::Kind;
	bool ok = true;
	auto require = [&](bool condition, char const* message) {
		if (!condition) { std::cerr << message << '\n'; ok = false; }
	};
	awst::ARC4UIntN word(256);
	awst::ARC4StaticArray small(&word, 129);
	awst::ARC4StaticArray large(&word, int64_t{1} << 27);
	awst::ARC4StaticArray overflow(&word, std::numeric_limits<int64_t>::max());
	awst::ARC4StaticArray negative(&word, -1);
	awst::ARC4DynamicArray dynamic(&word);
	awst::BytesWType empty(0);
	require(computeEncodedElementSize(&small).fixedBytes() == 4128, "small size changed");
	require(computeEncodedElementSize(&large).fixedBytes() == (uint64_t{1} << 32),
		"large fixed size wrapped");
	require(memoryUsesBlob(&large), "large type lost blob classification");
	require(computeEncodedElementSize(&overflow).kind == Kind::Overflow, "overflow not explicit");
	require(computeEncodedElementSize(&negative).kind == Kind::Unsupported, "negative length accepted");
	require(computeEncodedElementSize(&dynamic).kind == Kind::Dynamic, "dynamic type misclassified");
	require(computeEncodedElementSize(&empty).fixedBytes() == 0, "fixed zero lost");
	require(computeEncodedElementSize(awst::WType::arc4BoolType()).kind == Kind::Packed,
		"packed bool exposed a byte stride");
	awst::ARC4StaticArray bools(awst::WType::arc4BoolType(), std::numeric_limits<int64_t>::max());
	require(computeEncodedElementSize(&bools).fixedBytes() == (uint64_t{1} << 60),
		"packed bool ceiling addition overflowed");
	awst::ARC4Tuple pair({&large, &large});
	require(computeEncodedElementSize(&pair).fixedBytes() == (uint64_t{1} << 33),
		"aggregate sum narrowed");
	awst::ARC4Tuple mixed({&dynamic, &overflow});
	require(computeEncodedElementSize(&mixed).kind == Kind::Overflow,
		"dynamic component hid a later overflow");
	require(!arc4DefaultEncoding(&large), "oversized default was materialized");
	awst::ARC4StaticArray hugeDynamic(&dynamic, std::numeric_limits<int64_t>::max());
	require(!arc4DefaultEncoding(&hugeDynamic), "dynamic default allocation was not bounded");
	try
	{
		computeEncodedElementSize(&large).fixedBytes<int>();
		require(false, "unchecked fixed-size narrowing");
	}
	catch (SizeError const&) {}
	try
	{
		computeEncodedElementSize(&overflow).fixedBytes();
		require(false, "overflow fell back to a dynamic representation");
	}
	catch (SizeError const&) {}
	require(EncodedSize::fixed(std::numeric_limits<uint64_t>::max())
		.plus(EncodedSize::fixed(1)).kind == Kind::Overflow, "sum overflow missed");
	return ok ? 0 : 1;
}
