/// @file RevertBlob.h
/// EVM-compatible revert payloads logged before `err`.
/// AVM discards logs of failed txns on-chain, but simulate exposes them
/// (ARC-65-style); harness reads the last log entry.
///   Error(string):  0x08c379a0 ++ abi.encode(0x20, len, data…)
///   Panic(uint256): 0x4e487b71 ++ abi.encode(code)
#pragma once

#include "awst/Node.h"

#include <cstdint>
#include <string>
#include <vector>

namespace solidity::frontend { class FunctionCall; }
namespace puyasol::builder::eb { class ContractContext; }

namespace puyasol::builder::sol_ast
{

/// Shared payload construction for require, revert(string), and revert Error.
/// Error/Panic use EVM bytes; custom errors retain the private ARC4 transport.
struct RevertPayload
{
	std::string message = "assertion failed";
	std::shared_ptr<awst::Expression> blob;
	RevertPayload() = default;
	RevertPayload(eb::ContractContext& ctx, std::shared_ptr<awst::Expression> reason,
		awst::SourceLocation const& loc);
	RevertPayload(eb::ContractContext& ctx, solidity::frontend::FunctionCall const& error,
		awst::SourceLocation const& loc);
};

inline void appendRevertWord(std::vector<uint8_t>& _out, uint64_t _v)
{
	for (int i = 31; i >= 0; --i)
		_out.push_back(i < 8 ? static_cast<uint8_t>(_v >> (8 * i)) : 0);
}

/// Panic(uint256) blob — the code is always compile-time.
inline std::vector<uint8_t> panicRevertBlobBytes(uint64_t _code)
{
	std::vector<uint8_t> b = {0x4e, 0x48, 0x7b, 0x71};
	appendRevertWord(b, _code);
	return b;
}

/// `log(blob)` as a statement.
inline std::shared_ptr<awst::Statement> makeRevertLogStmt(
	std::shared_ptr<awst::Expression> _blob, awst::SourceLocation const& _loc)
{
	auto logCall = awst::makeIntrinsicCall("log", awst::WType::voidType(), _loc);
	logCall->stackArgs.push_back(std::move(_blob));
	return awst::makeExpressionStatement(std::move(logCall), _loc);
}

} // namespace puyasol::builder::sol_ast
