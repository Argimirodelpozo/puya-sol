#pragma once

#include "awst/Node.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <string>
#include <vector>

namespace puyasol::builder
{

/// Maps Solidity state variables to AWST AppStorageDefinitions.
class StorageMapper
{
public:
	explicit StorageMapper(TypeMapper& _typeMapper): m_typeMapper(_typeMapper) {}

	/// Create AppStorageDefinitions for a contract's state variables.
	std::vector<awst::AppStorageDefinition> mapStateVariables(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _sourceFile
	);

	/// Create an expression to read a state variable.
	std::shared_ptr<awst::Expression> createStateRead(
		std::string const& _varName,
		awst::WType const* _type,
		awst::AppStorageKind _kind,
		awst::SourceLocation const& _loc
	);

	/// Create an expression to write a state variable.
	std::shared_ptr<awst::Expression> createStateWrite(
		std::string const& _varName,
		std::shared_ptr<awst::Expression> _value,
		awst::WType const* _type,
		awst::AppStorageKind _kind,
		awst::SourceLocation const& _loc
	);

	/// Determine if a variable should use box storage.
	static bool shouldUseBoxStorage(solidity::frontend::VariableDeclaration const& _var);

	/// Compute the fixed encoded byte size of an AWST element type.
	/// Returns 0 for variable-length types (skip splitting).
	static int computeEncodedElementSize(awst::WType const* _type);

	/// Create a type-correct default value expression (0/false/empty) for the given wtype.
	static std::shared_ptr<awst::Expression> makeDefaultValue(
		awst::WType const* _type,
		awst::SourceLocation const& _loc
	);

private:
	TypeMapper& m_typeMapper;

	awst::SourceLocation makeLoc(
		solidity::langutil::SourceLocation const& _solLoc,
		std::string const& _file
	);

	std::shared_ptr<awst::BytesConstant> makeKeyExpr(
		std::string const& _name,
		awst::SourceLocation const& _loc,
		awst::AppStorageKind _kind = awst::AppStorageKind::AppGlobal
	);
};

} // namespace puyasol::builder
