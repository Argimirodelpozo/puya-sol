#pragma once

#include "builder/SolcFacts.h"

#include <libsolidity/ast/ASTAnnotations.h>
#include <libyul/AST.h>

namespace puyasol::builder
{

/// One owned, solc-disambiguated Yul tree. Identifier-keyed metadata and all
/// derived facts refer to this tree, whose address stays stable for the build.
struct PreparedAssembly
{
	solidity::yul::Block block;
	std::map<solidity::yul::Identifier const*,
		solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo>
		externalReferences;
	std::set<int64_t> assignedSlotDeclarations;
	SolcFacts::YulAnalysis facts;
};

} // namespace puyasol::builder
