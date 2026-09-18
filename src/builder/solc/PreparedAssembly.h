#pragma once

#include "builder/solc/SolcFacts.h"

#include <libsolidity/ast/ASTAnnotations.h>
#include <libyul/AST.h>

namespace puyasol::builder
{

/// One owned, solc-disambiguated Yul tree. Identifier-keyed metadata and all
/// derived facts refer to this tree, whose address stays stable for the build.
struct PreparedAssembly
{
	solidity::yul::Block block;
	solidity::yul::Dialect const* dialect = nullptr;
	std::map<solidity::yul::Identifier const*,
		solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo>
		externalReferences;
	std::set<int64_t> assignedSlotDeclarations;
	SolcFacts::YulAnalysis facts;
	// SSAValueTracker owns its default-zero expression; retain our own copy
	// so cached definitions never refer to a destroyed analysis visitor.
	solidity::yul::Expression zero{solidity::yul::Literal{
		{}, solidity::yul::LiteralKind::Number, solidity::yul::LiteralValue(solidity::u256{0})}};
	std::map<solidity::yul::YulName, solidity::yul::Expression const*> immutableDefinitions;
	std::map<solidity::yul::YulName, std::vector<solidity::yul::Expression const*>> incomingArguments;
};

} // namespace puyasol::builder
