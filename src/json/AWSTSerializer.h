#pragma once

#include "awst/Node.h"
#include "awst/WType.h"

#include <nlohmann/json.hpp>

namespace puyasol::json
{

/// Serializes AWST C++ nodes to JSON matching the format expected by `puya --awst`.
class AWSTSerializer
{
public:
	/// Serialize a list of root nodes to a JSON array.
	nlohmann::json serialize(std::vector<std::shared_ptr<awst::RootNode>> const& _roots);

	/// Serialize a single root node.
	nlohmann::json serializeRootNode(awst::RootNode const& _node);

	/// Serialize a contract.
	nlohmann::json serializeContract(awst::Contract const& _contract);

	/// Serialize a subroutine.
	nlohmann::json serializeSubroutine(awst::Subroutine const& _sub);

	/// Serialize a contract method.
	nlohmann::json serializeContractMethod(awst::ContractMethod const& _method);

	/// Serialize an expression.
	nlohmann::json serializeExpression(awst::Expression const& _expr);

	/// Serialize a statement.
	nlohmann::json serializeStatement(awst::Statement const& _stmt);

	/// Serialize a source location.
	nlohmann::json serializeSourceLocation(awst::SourceLocation const& _loc);

	/// Serialize a WType.
	nlohmann::json serializeWType(awst::WType const* _type);

	/// Serialize an ARC4 method config.
	nlohmann::json serializeARC4MethodConfig(awst::ARC4MethodConfig const& _config);

	/// Serialize a subroutine target.
	nlohmann::json serializeSubroutineTarget(awst::SubroutineTarget const& _target);

	/// Serialize a storage definition.
	nlohmann::json serializeAppStorageDefinition(awst::AppStorageDefinition const& _def);

	/// Serialize method documentation.
	nlohmann::json serializeMethodDocumentation(awst::MethodDocumentation const& _doc);

private:
	nlohmann::json serializeCallArg(awst::CallArg const& _arg);
	nlohmann::json serializeSubroutineArgument(awst::SubroutineArgument const& _arg);
	nlohmann::json serializeBlock(awst::Block const& _block);
};

} // namespace puyasol::json
