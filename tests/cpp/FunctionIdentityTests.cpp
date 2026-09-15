#include "builder/solc/FunctionIdentity.h"

#include <libsolidity/interface/CompilerStack.h>

#include <iostream>

int main()
{
	using namespace solidity::frontend;
	using namespace puyasol::builder;
	CompilerStack compiler;
	compiler.setSources({{"identity.sol", R"(
		function freeFunction() pure returns (uint) { return 1; }
		library L { function f() internal pure returns (uint) { return 2; } }
		abstract contract A { function absent() internal virtual; }
		contract C {
			constructor() {}
			function f() internal pure returns (uint) { return 3; }
			function g() private pure returns (uint) { return 4; }
			function entry() public pure returns (uint) { return f() + g(); }
		}
	)"}});
	if (!compiler.parseAndAnalyze()) return 1;
	bool ok = true;
	auto check = [&](FunctionDefinition const& function, bool root) {
		bool const named = function.name() != "entry" && function.name() != "absent"
			&& !function.isConstructor();
		auto symbol = functionSymbol(function);
		ok &= isRootSubroutine(function) == root && symbol.has_value() == named;
		if (symbol) ok &= *symbol == "__solfn_" + std::to_string(function.id());
	};
	for (auto const& node: compiler.ast("identity.sol").nodes())
		if (auto const* function = dynamic_cast<FunctionDefinition const*>(node.get()))
			check(*function, true);
		else if (auto const* contract = dynamic_cast<ContractDefinition const*>(node.get()))
			for (auto const* function: contract->definedFunctions()) check(*function, contract->isLibrary());
	if (!ok) std::cerr << "solc declaration-derived function identity mismatch\n";
	return ok ? 0 : 1;
}
