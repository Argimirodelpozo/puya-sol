#include "awst/WType.h"

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

	return ok ? 0 : 1;
}
