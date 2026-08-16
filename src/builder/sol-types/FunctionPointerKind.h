#pragma once

#include "builder/TargetProfile.h"

#include <libsolidity/ast/Types.h>

#include <cstddef>

namespace puyasol::builder
{

/// True iff `_funcType` is a function pointer that lives in the external-call
/// byte-string shape rather than the 8-byte
/// uint64 internal-dispatch-ID shape. The same predicate appears
/// in TypeMapper (return-type mapping), FunctionPointerBuilder (pointer
/// emission and call lowering), SolSelectorAccess (`.selector` extract),
/// and SolInternalCall (callsub vs inner-txn dispatch) — sharing it
/// here prevents drift if a new kind ever joins the external set.
///
/// Returns false for a null pointer (defensive default).
inline bool isExternalFunctionPointer(
	solidity::frontend::FunctionType const* _funcType)
{
	if (!_funcType) return false;
	auto kind = _funcType->kind();
	return kind == solidity::frontend::FunctionType::Kind::External
		|| kind == solidity::frontend::FunctionType::Kind::DelegateCall;
}

inline constexpr size_t externalFunctionPointerAddressBytes = 8;
inline constexpr size_t externalFunctionPointerSelectorBytes = 4;
inline constexpr size_t externalFunctionPointerSoliditySelectorOffset = 8;

/// Compatibility: appId[8] ++ routeSelector[4].
/// --evm-selectors: appId[8] ++ soliditySelector[4] ++ routeSelector[4].
/// Carrying both identities makes `.selector` faithful without losing the
/// information required by a later cross-contract ARC-4 call.
inline size_t externalFunctionPointerWidth(TargetProfile const& _profile)
{
	return _profile.evmSelectors ? 16 : 12;
}

inline size_t externalFunctionPointerRouteSelectorOffset(
	TargetProfile const& _profile)
{
	return _profile.evmSelectors ? 12 : 8;
}

} // namespace puyasol::builder
