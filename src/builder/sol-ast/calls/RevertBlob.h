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

namespace puyasol::builder::sol_ast
{

inline void appendRevertWord(std::vector<uint8_t>& _out, uint64_t _v)
{
	for (int i = 31; i >= 0; --i)
		_out.push_back(i < 8 ? static_cast<uint8_t>(_v >> (8 * i)) : 0);
}

/// Compile-time Error(string) blob for a constant message.
inline std::vector<uint8_t> errorStringRevertBlobBytes(std::string const& _msg)
{
	std::vector<uint8_t> b = {0x08, 0xc3, 0x79, 0xa0};
	appendRevertWord(b, 0x20);
	appendRevertWord(b, _msg.size());
	b.insert(b.end(), _msg.begin(), _msg.end());
	if (size_t rem = _msg.size() % 32; rem != 0)
		b.insert(b.end(), 32 - rem, 0);
	return b;
}

/// Panic(uint256) blob — the code is always compile-time.
inline std::vector<uint8_t> panicRevertBlobBytes(uint64_t _code)
{
	std::vector<uint8_t> b = {0x4e, 0x48, 0x7b, 0x71};
	appendRevertWord(b, _code);
	return b;
}

/// Runtime Error(string) blob: selector ++ offsetWord ++ leftPad32(len) ++ padded-data.
/// Empty string → 68-byte selector+offset+zero-length, no data chunk (matches EVM).
inline std::shared_ptr<awst::Expression> makeErrorStringRevertBlob(
	std::shared_ptr<awst::Expression> _msg, awst::SourceLocation const& _loc)
{
	if (_msg->wtype != awst::WType::bytesType())
		_msg = awst::makeAsBytes(std::move(_msg), _loc);
	// Used for both length word and data chunk — eval once.
	_msg = awst::makeEvalOnce(std::move(_msg), _loc);

	std::vector<uint8_t> head = {0x08, 0xc3, 0x79, 0xa0};
	appendRevertWord(head, 0x20);
	auto headConst = awst::makeBytesConstant(std::move(head), _loc);

	auto lenWord = awst::makeLeftPadToN(
		awst::makeItob(awst::makeLen(_msg, _loc), _loc), 32, _loc);

	// paddedLen = ((len+31)/32)*32; data = extract3(msg ++ bzero(31), 0, paddedLen)
	auto paddedLen = awst::makeUInt64BinOp(
		awst::makeUInt64BinOp(
			awst::makeUInt64BinOp(
				awst::makeLen(_msg, _loc), awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant("31", _loc), _loc),
			awst::UInt64BinaryOperator::FloorDiv,
			awst::makeIntegerConstant("32", _loc), _loc),
		awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant("32", _loc), _loc);
	auto data = awst::makeExtract3(
		awst::makeRightPad(_msg, 31, _loc),
		awst::makeIntegerConstant("0", _loc), std::move(paddedLen), _loc);

	return awst::makeConcat(
		awst::makeConcat(std::move(headConst), std::move(lenWord), _loc),
		std::move(data), _loc);
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
