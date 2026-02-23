#pragma once

#include "awst/Node.h"
#include "builder/ExpressionTranslator.h"
#include "builder/StatementTranslator.h"
#include "builder/StorageMapper.h"
#include "builder/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <memory>
#include <string>

namespace puyasol::builder
{

/// Translates a Solidity ContractDefinition into an AWST Contract node.
class ContractTranslator
{
public:
	ContractTranslator(
		TypeMapper& _typeMapper,
		StorageMapper& _storageMapper,
		std::string const& _sourceFile,
		LibraryFunctionIdMap const& _libraryFunctionIds
	);

	/// Translate a full contract definition.
	std::shared_ptr<awst::Contract> translate(
		solidity::frontend::ContractDefinition const& _contract
	);

private:
	TypeMapper& m_typeMapper;
	StorageMapper& m_storageMapper;
	std::string m_sourceFile;
	LibraryFunctionIdMap const& m_libraryFunctionIds;

	std::unique_ptr<ExpressionTranslator> m_exprTranslator;
	std::unique_ptr<StatementTranslator> m_stmtTranslator;
	OverloadedNamesSet m_overloadedNames;

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _solLoc);

	/// Build the approval program for the contract.
	awst::ContractMethod buildApprovalProgram(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName
	);

	/// Build the clear-state program.
	awst::ContractMethod buildClearProgram(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _contractName
	);

	/// Translate a function definition to a contract method.
	awst::ContractMethod translateFunction(
		solidity::frontend::FunctionDefinition const& _func,
		std::string const& _contractName
	);

	/// Build an ARC4 method config for a public/external function.
	std::optional<awst::ARC4MethodConfig> buildARC4Config(
		solidity::frontend::FunctionDefinition const& _func,
		awst::SourceLocation const& _loc
	);

	/// Inline modifier bodies into function body.
	void inlineModifiers(
		solidity::frontend::FunctionDefinition const& _func,
		std::shared_ptr<awst::Block>& _body
	);
};

} // namespace puyasol::builder
