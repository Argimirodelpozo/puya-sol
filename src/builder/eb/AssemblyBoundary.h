#pragma once

#include "builder/context/TranslationContext.h"

namespace solidity::frontend { class Block; }

namespace puyasol::builder
{

/// A Solidity local exposed to Yul keeps its word for the lifetime of the
/// declaration. Cleanup belongs to a typed read, never to assembly-block exit.
/// Same-type local copies transport the word without applying cleanup.
bool isAssemblyScalarCopy(awst::WType const* type);
std::shared_ptr<awst::Expression> assemblyScalarCopy(
	eb::ContractContext& ctx, solidity::frontend::VariableDeclaration const& destination,
	solidity::frontend::Expression const& source, awst::SourceLocation const& loc);

std::shared_ptr<awst::Expression> readAssemblyScalar(
	sol_ast::Context const& scope, TypeMapper& mapper,
	solidity::frontend::VariableDeclaration const& declaration,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out);

std::shared_ptr<awst::Statement> writeAssemblyScalar(
	sol_ast::Context const& scope, TypeMapper& mapper,
	solidity::frontend::VariableDeclaration const& declaration,
	std::shared_ptr<awst::Expression> value, awst::SourceLocation const& loc);

/// Register the declaration bindings before lowering any control-flow path.
/// Parameter/return word initialization dominates the entire emitted body.
void prepareAssemblyBoundary(sol_ast::FunctionContext& function,
	solidity::frontend::Block const& body,
	std::vector<std::shared_ptr<awst::Statement>>& prelude);

/// Physical internal-call companions; ABI entry signatures remain unchanged.
void appendCalldataParameters(CallBoundaryPlan const& plan,
	std::vector<awst::SubroutineArgument>& args, awst::SourceLocation const& loc);

} // namespace puyasol::builder
