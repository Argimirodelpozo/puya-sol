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
	nlohmann::ordered_json serialize(std::vector<std::shared_ptr<awst::RootNode>> const& _roots);

	/// Serialize a single root node.
	nlohmann::ordered_json serializeRootNode(awst::RootNode const& _node);

	/// Serialize a contract.
	nlohmann::ordered_json serializeContract(awst::Contract const& _contract);

	/// Serialize a subroutine.
	nlohmann::ordered_json serializeSubroutine(awst::Subroutine const& _sub);

	/// Serialize a contract method.
	nlohmann::ordered_json serializeContractMethod(awst::ContractMethod const& _method);

	/// Serialize an expression.
	nlohmann::ordered_json serializeExpression(awst::Expression const& _expr);

	/// Serialize a statement.
	nlohmann::ordered_json serializeStatement(awst::Statement const& _stmt);

	/// Serialize a source location.
	nlohmann::ordered_json serializeSourceLocation(awst::SourceLocation const& _loc);

	/// Serialize a WType.
	nlohmann::ordered_json serializeWType(awst::WType const* _type);

	/// Serialize an ARC4 method config.
	nlohmann::ordered_json serializeARC4MethodConfig(awst::ARC4MethodConfig const& _config);

	/// Serialize a subroutine target.
	nlohmann::ordered_json serializeSubroutineTarget(awst::SubroutineTarget const& _target);

	/// Serialize a storage definition.
	nlohmann::ordered_json serializeAppStorageDefinition(awst::AppStorageDefinition const& _def);

	/// Serialize method documentation.
	nlohmann::ordered_json serializeMethodDocumentation(awst::MethodDocumentation const& _doc);

private:
	nlohmann::ordered_json serializeCallArg(awst::CallArg const& _arg);
	nlohmann::ordered_json serializeSubroutineArgument(awst::SubroutineArgument const& _arg);
	nlohmann::ordered_json serializeBlock(awst::Block const& _block);
};

} // namespace puyasol::json
