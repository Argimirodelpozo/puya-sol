#pragma once

/// @file ReturnFinishing.h
/// Body-finishing steps shared by contract methods (ContractBuilder::buildFunction)
/// and freestanding/library subroutines (AWSTBuilder::buildFreestandingSubroutine):
/// named-return zero-inits + memory-return FMP bumps, and the implicit
/// fall-through return. The two callers differ only in the knobs below.

#include "awst/Node.h"
#include "builder/ReturnWirePlan.h"

#include <libsolidity/ast/ASTForward.h>

#include <cstddef>
#include <vector>

namespace puyasol::builder
{

class TypeMapper;
namespace sol_ast { struct FunctionContext; }

/// Zero-init named returns (Solidity implicit init) and bump the free-memory
/// pointer for every memory-typed return wider than `_memoryBumpMinBytes`
/// (EVM allocates them at entry; >4KB ones also bind their __blobagg_off_<id>
/// base first), inserting the statements at the front of `_body`.
/// `_skipValueInits`: a CHAIN-lowered modifier'd method threads its return
/// params through the chain (buildModifierChain) and the OUTER method zero-inits
/// them once — repeating it in the body would reset the value on every `_;` —
/// so only the FMP bumps remain.
void emitNamedReturnInits(
	awst::Block& _body,
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper& _typeMapper,
	bool _skipValueInits,
	int _memoryBumpMinBytes,
	awst::SourceLocation const& _loc);

/// Caller-specific knobs of emitImplicitReturn.
struct ImplicitReturnShape
{
	/// The callable yields a value at all (each caller's own test).
	bool hasReturnValue = false;
	/// Method: a named CALLDATA return whose pointer locals are live (an asm
	/// block wrote x.offset/x.length) reads through the pointer.
	bool calldataPointerReturns = false;
	/// Freestanding: a >4KB memory return is its __blobagg_off_<id> uint64.
	bool blobReturnsAsOffset = false;
	/// Method: an enum named return gets a range assert.
	bool enumRangeAssert = false;
	/// Method: build-time wire encoding of the synthesized value.
	bool encodeReturns = false;
	std::vector<ReturnWireElem> const* returnPlan = nullptr;
	bool asmWrap = false;
};

/// Append the implicit fall-through return when `_body` can fall off the end:
/// the augmented args (void + augmentation), the named-return values, or the
/// default zero of `_returnType`.
void emitImplicitReturn(
	awst::Block& _body,
	awst::WType const* _returnType,
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper& _typeMapper,
	sol_ast::FunctionContext const& _fnCtx,
	ImplicitReturnShape const& _shape,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder
