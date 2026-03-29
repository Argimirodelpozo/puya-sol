#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <string>
#include <unordered_map>
#include <functional>

namespace puyasol::builder::eb
{

/// Registry of Solidity builtin function callables.
///
/// Maps function names (require, keccak256, sha256, etc.) to callable builder
/// factories. Used by visit(FunctionCall) to dispatch builtins through the
/// builder pattern instead of if-else chains.
class BuiltinCallableRegistry
{
public:
	using CallHandler = std::function<std::unique_ptr<InstanceBuilder>(
		BuilderContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc)>;

	BuiltinCallableRegistry();

	/// Try to handle a builtin function call by name.
	/// Returns nullptr if the name is not a registered builtin.
	std::unique_ptr<InstanceBuilder> tryCall(
		BuilderContext& _ctx,
		std::string const& _name,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc) const;

private:
	std::unordered_map<std::string, CallHandler> m_handlers;

	void registerHandler(std::string _name, CallHandler _handler);

	// Individual handlers
	static std::unique_ptr<InstanceBuilder> handleKeccak256(
		BuilderContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleSha256(
		BuilderContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleMulmod(
		BuilderContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleAddmod(
		BuilderContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleGasleft(
		BuilderContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleSelfdestruct(
		BuilderContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	/// Promote uint64 to biguint.
	static std::shared_ptr<awst::Expression> promoteToBigUInt(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
