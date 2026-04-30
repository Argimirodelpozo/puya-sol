#pragma once

/// @file AbiCodecHelpers.h
/// Internal helpers shared between AbiEncoderBuilder.cpp and AbiCodecImpl.cpp.
/// Header-only so each TU gets its own copy.

#include "awst/Node.h"

#include <memory>
#include <string>

namespace puyasol::builder::eb::abi_codec
{

/// Fresh-name counter shared across all loop emitters. Inline so all TUs
/// see the same definition; each encoder call produces unique local var
/// names that won't collide across multiple encoders in the same function
/// body.
inline int s_encLoopCounter = 0;

inline std::shared_ptr<awst::AssignmentStatement> assignFresh(
	std::shared_ptr<awst::Expression> _target,
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc)
{
	return awst::makeAssignmentStatement(std::move(_target), std::move(_value), _loc);
}

inline std::shared_ptr<awst::Expression> u64Const(std::string const& _v, awst::SourceLocation const& _loc)
{
	return awst::makeIntegerConstant(_v, _loc);
}

/// concat(a, b) on bytes
inline std::shared_ptr<awst::Expression> bytesConcat(
	std::shared_ptr<awst::Expression> _a,
	std::shared_ptr<awst::Expression> _b,
	awst::SourceLocation const& _loc)
{
	auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	cat->stackArgs.push_back(std::move(_a));
	cat->stackArgs.push_back(std::move(_b));
	return cat;
}

/// extract3(bytes, start, length) on bytes
inline std::shared_ptr<awst::Expression> bytesExtract3(
	std::shared_ptr<awst::Expression> _bytes,
	std::shared_ptr<awst::Expression> _start,
	std::shared_ptr<awst::Expression> _length,
	awst::SourceLocation const& _loc)
{
	auto e = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	e->stackArgs.push_back(std::move(_bytes));
	e->stackArgs.push_back(std::move(_start));
	e->stackArgs.push_back(std::move(_length));
	return e;
}

/// extract_uint16(bytes, byte_offset) → uint64
inline std::shared_ptr<awst::Expression> bytesExtractU16(
	std::shared_ptr<awst::Expression> _bytes,
	std::shared_ptr<awst::Expression> _offset,
	awst::SourceLocation const& _loc)
{
	auto e = awst::makeIntrinsicCall("extract_uint16", awst::WType::uint64Type(), _loc);
	e->stackArgs.push_back(std::move(_bytes));
	e->stackArgs.push_back(std::move(_offset));
	return e;
}

/// len(bytes) → uint64
inline std::shared_ptr<awst::Expression> bytesLen(
	std::shared_ptr<awst::Expression> _bytes,
	awst::SourceLocation const& _loc)
{
	auto e = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
	e->stackArgs.push_back(std::move(_bytes));
	return e;
}

/// itob(uint64) → bytes (8 bytes BE)
inline std::shared_ptr<awst::Expression> u64Itob(
	std::shared_ptr<awst::Expression> _v,
	awst::SourceLocation const& _loc)
{
	auto e = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
	e->stackArgs.push_back(std::move(_v));
	return e;
}

/// bzero(n) → bytes of n zero bytes
inline std::shared_ptr<awst::Expression> bytesBzero(
	std::shared_ptr<awst::Expression> _n,
	awst::SourceLocation const& _loc)
{
	auto e = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	e->stackArgs.push_back(std::move(_n));
	return e;
}

} // namespace puyasol::builder::eb::abi_codec
