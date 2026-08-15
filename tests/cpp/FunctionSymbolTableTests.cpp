#include "builder/FunctionSymbolTable.h"

#include <iostream>

namespace
{

bool require(bool _condition, char const* _message)
{
	if (_condition)
		return true;
	std::cerr << _message << '\n';
	return false;
}

} // namespace

int main()
{
	using puyasol::builder::FunctionSymbolTable;
	bool ok = true;

	FunctionSymbolTable symbols;
	auto const& root = symbols.registerDeclaration(11, true);
	auto const& method = symbols.registerDeclaration(12, false);
	ok &= require(root == "__solfn_11", "root symbol is not declaration-based");
	ok &= require(method == "__solfn_12", "method symbol is not declaration-based");
	ok &= require(symbols.resolve(11) && *symbols.resolve(11) == root,
		"registered root declaration cannot be resolved");
	ok &= require(symbols.resolve(12) && *symbols.resolve(12) == method,
		"registered method declaration cannot be resolved");
	ok &= require(symbols.isRootSubroutine(11), "root classification was lost");
	ok &= require(!symbols.isRootSubroutine(12), "contract method was classified as a root");

	// A later registration can discover that a declaration needs a root AWST
	// subroutine, but it must never change the declaration's stable symbol.
	auto const& promoted = symbols.registerDeclaration(12, true);
	ok &= require(promoted == method, "re-registration changed the declaration symbol");
	ok &= require(symbols.isRootSubroutine(12), "root promotion was not retained");

	symbols.clear();
	ok &= require(!symbols.resolve(11) && !symbols.resolve(12),
		"clear retained declaration symbols");
	ok &= require(!symbols.isRootSubroutine(11) && !symbols.isRootSubroutine(12),
		"clear retained root classifications");

	return ok ? 0 : 1;
}
