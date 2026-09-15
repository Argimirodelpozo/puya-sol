#include "builder/codec/Arc4Defaults.h"

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
	require(memoryUsesBlob(TargetProfile{}, &large), "large type lost blob classification");
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
	awst::ARC4UIntN byte(8);
	awst::ARC4StaticArray boolRun(awst::WType::arc4BoolType(), 9);
	std::vector<awst::WType const*> fields(9, awst::WType::arc4BoolType());
	fields.push_back(&byte);
	fields.push_back(awst::WType::stringType());
	awst::ARC4Tuple boolsAndString(fields);
	require(arc4DefaultEncoding(&boolsAndString) == std::vector<uint8_t>({0, 0, 0, 0, 5, 0, 0}),
		"packed bool run or string tail offset is invalid");
	std::vector<std::pair<std::string, awst::WType const*>> packedFields{{"tag", &byte}};
	for (int i = 0; i < 9; ++i)
		packedFields.emplace_back("flag" + std::to_string(i), awst::WType::arc4BoolType());
	packedFields.emplace_back("value", &word);
	packedFields.emplace_back("last", awst::WType::arc4BoolType());
	awst::ARC4Struct packedStruct("Packed", packedFields);
	require(arc4FieldBitOffsets(packedStruct) == std::vector<uint64_t>(
		{0, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24, 280}),
		"struct field offsets disagreed with packed bool runs");
	awst::ARC4Struct emptyStruct("Empty", {});
	require(arc4FieldBitOffsets(emptyStruct) == std::vector<uint64_t>{},
		"empty struct layout is not fixed");
	awst::ARC4Struct dynamicStruct("Dynamic", {{"values", &dynamic}});
	require(!arc4FieldBitOffsets(dynamicStruct), "dynamic struct exposed fixed offsets");
	try
	{
		awst::ARC4StaticArray hugeBytes(&byte, std::numeric_limits<int64_t>::max());
		awst::ARC4Struct bitOverflow("Overflow", {{"bytes", &hugeBytes}, {"last", &byte}});
		arc4FieldBitOffsets(bitOverflow);
		require(false, "struct field bit offset overflow was not rejected");
	}
	catch (SizeError const&) {}
	for (auto const* type: std::vector<awst::WType const*>{&word, &small, &boolRun, &empty,
		&dynamic, &boolsAndString, awst::WType::stringType(), awst::WType::boolType()})
	{
		auto size = computeEncodedElementSize(type);
		require(arc4IsDynamic(type) == (size.kind == Kind::Dynamic), "dynamic classifiers disagreed");
		auto bytes = arc4DefaultEncoding(type);
		require(bytes.has_value(), "supported default is missing");
		if (auto fixed = size.fixedBytes())
			require(bytes && bytes->size() == *fixed, "fixed default disagreed with encoded size");
	}
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
