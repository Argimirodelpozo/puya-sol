#include "json/AWSTSerializer.h"
#include <boost/multiprecision/cpp_int.hpp>
#include <iostream>

int main()
{
	using namespace puyasol;
	json::AWSTSerializer serializer;
	bool ok = true;
	auto require = [&](bool condition, char const* message) {
		if (!condition) { std::cerr << message << '\n'; ok = false; }
	};
	auto value = [&](std::string const& text) {
		return serializer.serializeExpression(*awst::makeIntegerConstant(text, {}))["value"];
	};
	for (auto text: {"0", "-0", "+0", "000", "0x0", "-0X00"})
		require(value(text) == 0, "zero is not canonical");
	require(value("00019") == 19, "decimal was parsed as octal");
	require(value("+0X7f") == 127, "hex prefix/sign not accepted");
	for (unsigned bits: {63u, 64u, 255u, 256u, 511u, 512u})
		for (int delta: {-1, 0, 1})
			for (int sign: {-1, 1})
			{
				boost::multiprecision::cpp_int magnitude = (boost::multiprecision::cpp_int(1) << bits) + delta;
				boost::multiprecision::cpp_int number = magnitude * sign;
				std::string decimal = number.str();
				std::string hex = (sign < 0 ? "-0x" : "0x") + magnitude.str(0, std::ios_base::hex);
				auto result = value(decimal);
				require(result == value(hex), "hex and decimal serialization disagree");
				bool small = number >= std::numeric_limits<int64_t>::min() && number <= std::numeric_limits<int64_t>::max();
				require(result.is_number_integer() == small, "integer/string boundary changed");
				if (!small) require(result == decimal, "large integer is not decimal");
			}
	for (auto text: {"", "-", "+", "0x", "-0x", "0xg", "12tail", " 1", "1 ", "1.0", "--1", "0x1g"})
		try { value(text); require(false, "malformed integer accepted"); }
		catch (std::invalid_argument const&) {}
	auto checkInvalid = [&](auto expression) {
		try { serializer.serializeExpression(*expression); require(false, "invalid operator accepted"); }
		catch (std::logic_error const&) {}
	};
	checkInvalid(awst::makeUInt64BinOp(awst::makeOne({}), static_cast<awst::UInt64BinaryOperator>(999), awst::makeOne({}), {}));
	checkInvalid(awst::makeNumericCompare(awst::makeOne({}), static_cast<awst::NumericComparison>(999), awst::makeOne({}), {}));
	struct Unknown: awst::RootNode { std::string nodeType() const override { return "Unknown"; } } root;
	try { serializer.serializeRootNode(root); require(false, "unknown root accepted"); }
	catch (std::logic_error const&) {}
	return ok ? 0 : 1;
}
