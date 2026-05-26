#pragma once

#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

/// True iff `_funcType` is a function pointer that lives in the 12-byte
/// `(appId[8], selector[4])` external-call shape rather than the 8-byte
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

} // namespace puyasol::builder
