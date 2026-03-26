#pragma once

#include "awst/Node.h"
#include "builder/expressions/ExpressionBuilder.h"
#include "builder/statements/StatementBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <memory>
#include <optional>
#include <string>

namespace puyasol::builder
{

/// Builds an AWST Contract node from a Solidity ContractDefinition.
///
/// Orchestrates the translation of a complete contract including:
///   - Approval and clear-state programs
///   - Public/external methods with ARC4 method configs
///   - Constructor with inheritance specifier arguments
///   - Modifier inlining into function bodies
///   - Super call resolution across inheritance hierarchy
///   - Automatic __postInit generation for box-writing constructors
class ContractBuilder
{
public:
	ContractBuilder(
		TypeMapper& _typeMapper,
		StorageMapper& _storageMapper,
		std::string const& _sourceFile,
		LibraryFunctionIdMap const& _libraryFunctionIds,
		uint64_t _opupBudget = 0,
		FreeFunctionIdMap const& _freeFunctionById = {}
	);

	/// Build AWST from a full contract definition.
	std::shared_ptr<awst::Contract> build(
		solidity::frontend::ContractDefinition const& _contract
	);

private:
	TypeMapper& m_typeMapper;
	StorageMapper& m_storageMapper;
	std::string m_sourceFile;
	LibraryFunctionIdMap const& m_libraryFunctionIds;
	uint64_t m_opupBudget = 0;
	FreeFunctionIdMap const& m_freeFunctionById;

	std::unique_ptr<ExpressionBuilder> m_exprBuilder;
	std::unique_ptr<StatementBuilder> m_stmtBuilder;
	OverloadedNamesSet m_overloadedNames;

	/// The contract currently being built (for modifier override resolution).
	solidity::frontend::ContractDefinition const* m_currentContract = nullptr;

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

	/// Build a contract method from a function definition.
	awst::ContractMethod buildFunction(
		solidity::frontend::FunctionDefinition const& _func,
		std::string const& _contractName,
		std::string const& _nameOverride = ""
	);

	/// Sign-extend a value from N-bit signed integer to 256-bit two's complement.
	/// Masks to N bits, then if sign bit is set, adds (2^256 - 2^N) mod 2^256.
	static std::shared_ptr<awst::Expression> signExtendToUint256(
		std::shared_ptr<awst::Expression> _value,
		unsigned _bits,
		awst::SourceLocation const& _loc
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

	/// If buildApprovalProgram detects box writes in the constructor,
	/// it populates this with an auto-generated __postInit method.
	std::optional<awst::ContractMethod> m_postInitMethod;
};

} // namespace puyasol::builder
