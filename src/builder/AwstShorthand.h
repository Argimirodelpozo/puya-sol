#pragma once

/// @file AwstShorthand.h
/// Tiny AWST construction shorthands shared by the ABI/codec/entry emitters.
/// These used to be re-declared per .cpp in anonymous namespaces (u64 ×4,
/// add ×3, byte-identical) — one drifting copy per file. Pull them in with
/// `using namespace puyasol::builder::shorthand;` inside the consuming
/// namespace; the emitted nodes are identical to the old locals.

#include "awst/Node.h"

#include <variant>

namespace puyasol::builder::shorthand
{

inline std::shared_ptr<awst::Expression> u64(
	uint64_t value, awst::SourceLocation const& loc)
{
	return awst::makeIntegerConstant(value, loc);
}

inline std::shared_ptr<awst::Expression> u64(
	std::string const& value, awst::SourceLocation const& loc)
{
	return awst::makeIntegerConstant(value, loc);
}

inline std::shared_ptr<awst::Expression> add(
	std::shared_ptr<awst::Expression> left,
	std::shared_ptr<awst::Expression> right,
	awst::SourceLocation const& loc)
{
	return awst::makeUInt64BinOp(std::move(left),
		awst::UInt64BinaryOperator::Add, std::move(right), loc);
}

inline std::shared_ptr<awst::Expression> u64Var(
	std::string const& name, awst::SourceLocation const& loc)
{
	return awst::makeVarExpression(name, awst::WType::uint64Type(), loc);
}

inline std::shared_ptr<awst::Expression> bytesVar(
	std::string const& name, awst::SourceLocation const& loc)
{
	return awst::makeVarExpression(name, awst::WType::bytesType(), loc);
}

inline std::shared_ptr<awst::Expression> biguintConst(
	std::string const& value, awst::SourceLocation const& loc)
{
	return awst::makeIntegerConstant(value, loc, awst::WType::biguintType());
}

/// The compiler's self-address convention: the expression is exactly
/// `global CurrentApplicationAddress`. Receivers/bases matching this lower
/// self-calls to direct callsubs / CurrentApplicationID lookups.
/// (Previously ~6 per-file copies of the same IntrinsicCall sniff.)
inline bool isCurrentAppAddressGlobal(awst::Expression const* _e)
{
	auto const* ic = dynamic_cast<awst::IntrinsicCall const*>(_e);
	return ic && ic->opCode == "global"
		&& !ic->immediates.empty()
		&& std::holds_alternative<std::string>(ic->immediates[0])
		&& std::get<std::string>(ic->immediates[0]) == "CurrentApplicationAddress";
}

} // namespace puyasol::builder::shorthand
