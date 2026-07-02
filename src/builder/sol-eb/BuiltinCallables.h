#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <string>
#include <unordered_map>
#include <functional>

namespace puyasol::builder::eb
{

/// Dispatch table for Solidity builtin functions (keccak256, sha256, mulmod, etc.).
class BuiltinCallableRegistry
{
public:
	using CallHandler = std::function<std::unique_ptr<InstanceBuilder>(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc)>;

	BuiltinCallableRegistry();

	/// Try to handle a builtin function call by name.
	/// Returns nullptr if the name is not a registered builtin.
	std::unique_ptr<InstanceBuilder> tryCall(
		ContractContext& _ctx,
		std::string const& _name,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc) const;

private:
	std::unordered_map<std::string, CallHandler> m_handlers;

	void registerHandler(std::string _name, CallHandler _handler);

	// Individual handlers
	static std::unique_ptr<InstanceBuilder> handleKeccak256(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleSha256(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleMulmod(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleAddmod(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleGasleft(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleSelfdestruct(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleEcrecover(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	// Biguint promotion is the shared eb::promoteToBiguint (BigUIntMathHelpers).
};

} // namespace puyasol::builder::eb
