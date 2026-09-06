#pragma once

/// @file HelperMethod.h
/// One constructor for synthesized contract methods (router arms, storage
/// dispatchers, modifier chain subs, EVM decode helpers): the 30+ hand-built
/// `awst::ContractMethod` sites each repeated the same six field assignments
/// plus a per-argument `SubroutineArgument` block. Pure construction; callers
/// still attach the body.

#include "awst/Node.h"

#include <string>
#include <utility>
#include <vector>

namespace puyasol::awst
{

/// (name, wtype) pairs become SubroutineArguments at `loc`.
using HelperArgs = std::vector<std::pair<std::string, WType const*>>;

inline SubroutineArgument makeHelperArg(
	std::string _name, WType const* _wtype, SourceLocation const& _loc)
{
	SubroutineArgument arg;
	arg.name = std::move(_name);
	arg.wtype = _wtype;
	arg.sourceLocation = _loc;
	return arg;
}

/// A method with no ARC-4 routing config (`arc4MethodConfig = nullopt`), the
/// given args and return type, and an empty block body ready to fill.
inline ContractMethod makeHelperMethod(
	std::string _cref,
	std::string _memberName,
	WType const* _returnType,
	HelperArgs const& _args,
	SourceLocation const& _loc)
{
	ContractMethod method;
	method.sourceLocation = _loc;
	method.cref = std::move(_cref);
	method.memberName = std::move(_memberName);
	method.returnType = _returnType;
	method.arc4MethodConfig = std::nullopt;
	for (auto const& [name, wtype]: _args)
		method.args.push_back(makeHelperArg(name, wtype, _loc));
	method.body = makeBlock(_loc);
	return method;
}

} // namespace puyasol::awst
