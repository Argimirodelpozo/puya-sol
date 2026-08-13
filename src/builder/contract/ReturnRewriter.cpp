#include "builder/contract/ReturnRewriter.h"
#include "awst/StatementWalk.h"
#include "awst/NameGen.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/TypeMapper.h"

#include <functional>
#include <set>
#include <string>

namespace puyasol::builder
{

void forEachReturnStatement(
	std::vector<std::shared_ptr<awst::Statement>>& _stmts,
	std::function<void(awst::ReturnStatement&)> const& _fn)
{
	// Containers via awst::forEachChildBlock — THE single enumeration (T5).
	for (auto& stmt: _stmts)
	{
		if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
			_fn(*ret);
		else
			awst::forEachChildBlock(*stmt, [&](awst::Block& b, bool) {
				forEachReturnStatement(b.body, _fn);
			});
	}
}

/// Compute the wire plan for every return element (declared in ReturnRewriter.h;
/// shared with the build-time encoder in SolReturnStatement). Reads element native
/// types from `_returnType` (the promoted method.returnType: signed sub-64 and
/// >64-bit ints are already biguint there) — NOT a fresh map() of the Solidity type,
/// which would give int64→uint64 and miss the promotion. Solidity types supply
/// signedness + declared bits.
std::vector<ReturnWireElem> computeReturnPlan(
	solidity::frontend::FunctionDefinition const& _func,
	awst::WType const* _returnType,
	TypeMapper& _tm)
{
	auto const& returnParams = _func.returnParameters();
	auto const* retTuple = (_returnType && _returnType->kind() == awst::WTypeKind::WTuple)
		? static_cast<awst::WTuple const*>(_returnType) : nullptr;
	std::vector<ReturnWireElem> plan;
	for (size_t i = 0; i < returnParams.size(); ++i)
	{
		ReturnWireElem p;
		p.nativeType = retTuple
			? (i < retTuple->types().size() ? retTuple->types()[i] : nullptr)
			: _returnType;
		p.wireType = p.nativeType;
		if (p.nativeType == awst::WType::biguintType())
		{
			auto si = SolIntType::fromSolOrEnum(returnParams[i]->type());
			p.isSigned = si && si->isSigned;
			// bits = DECLARED width (drives sign-extension, sub-word mask, asm mod-wrap);
			// the WIRE width is 256 for signed (full two's complement) else the declared
			// width. Keep these distinct — the sign-extension needs the declared 64, not 256.
			p.bits = si ? si->bits : 256u;
			unsigned wireWidth = p.isSigned ? 256u : p.bits;
			p.wireType = _tm.createType<awst::ARC4UIntN>(static_cast<int>(wireWidth));
			p.encoded = true;
		}
		else if (auto si = SolIntType::fromSolOrEnum(returnParams[i]->type());
			si && !si->isSigned && si->bits < 64)
		{
			// Unsigned sub-64 (native uint64): mask to the declared width — the wire
			// type stays uint64. Same condition FunctionBuilder uses for unsignedMasks
			// (Pass 5); consumed only by the build-time encoder (chain still uses Pass 5).
			p.masked = true;
			p.bits = si->bits;
		}
		else if (p.nativeType
			&& p.nativeType->kind() == awst::WTypeKind::ReferenceArray)
		{
			// Dynamic-array return → its ARC4 array type + ARC4Encode (Pass 1).
			auto const* arc4 = _tm.mapToARC4Type(p.nativeType);
			if (arc4 != p.nativeType)
			{
				p.wireType = arc4;
				p.encoded = true;
			}
		}
		plan.push_back(p);
	}
	return plan;
}

/// ── THE WIRE-RETURN-TYPE SPEC (D2, 2026-07-10) ───────────────────────────────
/// Per-return-element ABI wire type (oracle fixture: tests/WIP/return-wire-oracle/
/// return_matrix.sol + oracle.txt — regenerate and diff before/after ANY change):
///   unsigned intN, N<=64           → native uint64 (sub-word masked to N)
///   unsigned intN, 64<N<=256       → arc4.uintN
///   SIGNED intN, any width         → arc4.uint256 (sign-extended two's complement)
///   bool / address / byte[N]       → native (untouched)
///   dynamic bytes / string         → native bytes/string (untouched)
///   T[] (ReferenceArray)           → arc4 array
///   tuple (static OR dynamic elem) → per-element by the rules above
///   asm-bodied biguint             → arc4.uintN with `% 2^N` wrap (Yul is unchecked)
///
/// WHERE the encoding happens (fable-review-2 D2 build-time-return-encoding):
///  - NON-MODIFIER (non-chain) methods: encoded AT CONSTRUCTION — SolReturnStatement
///    and FunctionBuilder's synthesized return call TypeCoercion::encodeReturnValue with
///    the plan from computeReturnPlan (stashed in FunctionContext). FunctionBuilder then
///    SKIPS this post-pass entirely. (Old passes 2 & 3 — scalar/tuple biguint — are gone.)
///  - MODIFIER'D (chain-lowered) methods: the chain threads NATIVE values through its
///    subs, so encoding is deferred to encodeChainDispatchReturn (called AFTER
///    buildModifierChain — a legitimately later phase). The passes BELOW therefore run
///    ONLY for chain-lowered functions: Pass 1 (array), Pass 4 (signed sign-extend into
///    the body), Pass 5 (sub-word mask), Pass 6 (coerce a modifier's named-return var).
/// computeReturnPlan is the ONE per-element wire-type decision, shared by both paths.
void rewriteARC4Returns(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper& m_typeMapper,
	std::vector<SignedReturnInfo> const& signedReturns,
	std::vector<UnsignedMaskInfo> const& unsignedMasks)
{
	auto const& returnParams = _func.returnParameters();

	// Pass 1: dynamic-array returns → ARC4 type + ARC4Encode wrap.
	if (method.arc4MethodConfig.has_value()
		&& method.returnType->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* arc4RetType = m_typeMapper.mapToARC4Type(method.returnType);
		if (arc4RetType != method.returnType)
		{
			// Wrap all return values in ARC4Encode
			forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
				if (ret.value)
				{
					auto loc = ret.value->sourceLocation;
					auto encode = awst::makeARC4Encode(std::move(ret.value), arc4RetType, loc);
					ret.value = std::move(encode);
				}
			});
			method.returnType = arc4RetType;
		}
	}

	// Passes 2 & 3 (non-chain scalar/tuple biguint encoding) were DELETED — that
	// encoding now happens at BUILD TIME (SolReturnStatement / FunctionBuilder via
	// TypeCoercion::encodeReturnValue, fable-review-2 D2). A non-modifier ABI method
	// never reaches this post-pass (FunctionBuilder skips it); what remains below runs
	// only for CHAIN-lowered (modifier'd) functions + the array/mask/coerce passes.

	// Pass 4 (CHAIN-ONLY): sign-extend signed return elements to 256-bit two's
	// complement IN THE BODY, so they thread through the modifier chain's subs as
	// biguint (the subs' return slots are the promoted native types). The ABI
	// encoding of the outer dispatch return is done afterwards by
	// encodeChainDispatchReturn — so NO ARC4Encode / retype here (that would make a
	// sub return arc4 where it's declared native). Non-chain signed returns are
	// encoded at build time (SolReturnStatement), so this pass isn't reached for them.
	if (!signedReturns.empty() && method.arc4MethodConfig.has_value())
	{
		auto signExt = [&](std::shared_ptr<awst::Expression> _v, unsigned _bits,
			awst::SourceLocation const& _loc) {
			return TypeCoercion::signExtendToUint256(std::move(_v), _bits, _loc);
		};

		forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
			if (!ret.value) return;
			auto srcLoc = ret.value->sourceLocation;

			if (signedReturns.size() == 1 && signedReturns[0].index == 0
				&& returnParams.size() == 1)
			{
				ret.value = signExt(std::move(ret.value), signedReturns[0].bits, srcLoc);
			}
			else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret.value.get()))
			{
				for (auto const& sr: signedReturns)
					if (sr.index < tuple->items.size())
						tuple->items[sr.index] = signExt(
							std::move(tuple->items[sr.index]), sr.bits, srcLoc);
				tuple->wtype = method.returnType;
			}
			else if (returnParams.size() > 1)
			{
				// Opaque tuple value (`return f()`): can't reach elements in place, so
				// spill to a temp (eval once — the value may be a side-effecting call)
				// and rebuild, sign-extending the signed elements.
				if (auto const* nativeTuple = dynamic_cast<awst::WTuple const*>(ret.value->wtype))
				{
					auto const* tupleWType = ret.value->wtype;
					std::string tn = "__rettuple_" + std::to_string(
						(awst::NameGen::next("ReturnRewriter.s_retTupleId") + 1));
					auto bind = awst::makeAssignmentExpression(
						awst::makeVarExpression(tn, tupleWType, srcLoc),
						std::move(ret.value), srcLoc, tupleWType);
					auto rebuilt = awst::makeTupleExpression(method.returnType, srcLoc);
					for (size_t i = 0; i < nativeTuple->types().size(); ++i)
					{
						std::shared_ptr<awst::Expression> item = awst::makeTupleItem(
							awst::makeVarExpression(tn, tupleWType, srcLoc),
							static_cast<int>(i), nativeTuple->types()[i], srcLoc);
						for (auto const& sr: signedReturns)
							if (sr.index == i)
							{
								item = signExt(std::move(item), sr.bits, srcLoc);
								break;
							}
						rebuilt->items.push_back(std::move(item));
					}
					rebuilt->wtype = method.returnType;
					auto comma = awst::makeCommaExpression(method.returnType, srcLoc);
					comma->expressions.push_back(std::move(bind));
					comma->expressions.push_back(std::move(rebuilt));
					ret.value = std::move(comma);
				}
			}
		});
	}

	// Pass 5: unsigned sub-word returns → mask to declared width (AVM preserves full uint64).
	if (!unsignedMasks.empty() && method.arc4MethodConfig.has_value())
	{
		auto maskValue = [&](std::shared_ptr<awst::Expression> val,
			unsigned bits, awst::SourceLocation const& loc)
			-> std::shared_ptr<awst::Expression>
		{
			uint64_t mask = (uint64_t(1) << bits) - 1;
			auto maskConst = awst::makeIntegerConstant(mask, loc);
			auto bitAnd = awst::makeUInt64BinOp(std::move(val), awst::UInt64BinaryOperator::BitAnd, std::move(maskConst), loc);
			return bitAnd;
		};

		forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
			if (!ret.value) return;
			auto srcLoc = ret.value->sourceLocation;
			if (unsignedMasks.size() == 1 && unsignedMasks[0].index == 0
				&& returnParams.size() == 1)
			{
				ret.value = maskValue(std::move(ret.value),
					unsignedMasks[0].bits, srcLoc);
			}
			else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret.value.get()))
			{
				for (auto const& um: unsignedMasks)
				{
					if (um.index < tuple->items.size())
						tuple->items[um.index] = maskValue(
							std::move(tuple->items[um.index]), um.bits, srcLoc);
				}
			}
		});
	}

	// Pass 6 (safety): coerce native-int return values to match method.returnType.
	// Some paths leave uint64↔biguint mismatches that puya rejects; e.g. a
	// signed sub-word sign-extended to biguint while a modifier moves it into a
	// uint64 named-return var (V4 PoolManager.initialize int24 tick phi).
	if (method.returnType
		&& (method.returnType == awst::WType::uint64Type()
			|| method.returnType == awst::WType::biguintType()))
	{
		auto isNativeInt = [](awst::WType const* t) { return awst::isNumericWType(t); };
		// Name of the single named-return var, if any. A modifier moves the
		// return value into this var (so the body ends with `<name> = <value>`
		// instead of `return <value>`); we must coerce that assignment too.
		// Also coerce assignments into the single named-return var (modifier path).
		std::string const namedRet =
			returnParams.size() == 1 ? returnParams[0]->name() : std::string{};
		std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> walkCoerce;
		walkCoerce = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
		{
			for (auto& stmt: stmts)
			{
				if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
				{
					if (ret->value && ret->value->wtype && isNativeInt(ret->value->wtype)
						&& ret->value->wtype != method.returnType)
					{
						auto loc = ret->value->sourceLocation;
						ret->value = TypeCoercion::implicitNumericCast(
							std::move(ret->value), method.returnType, loc);
					}
				}
				else if (auto* as = dynamic_cast<awst::AssignmentStatement*>(stmt.get()))
				{
					// Coerce assignment into named-return var; retype target for avm_type consistency.
					if (auto* tv = dynamic_cast<awst::VarExpression*>(as->target.get()))
						if (!namedRet.empty() && tv->name == namedRet && as->value
							&& as->value->wtype && isNativeInt(as->value->wtype)
							&& as->value->wtype != method.returnType)
						{
							auto loc = as->value->sourceLocation;
							as->value = TypeCoercion::implicitNumericCast(
								std::move(as->value), method.returnType, loc);
							tv->wtype = method.returnType;
						}
				}
				else if (auto* ie = dynamic_cast<awst::IfElse*>(stmt.get()))
				{
					if (ie->ifBranch) walkCoerce(ie->ifBranch->body);
					if (ie->elseBranch) walkCoerce(ie->elseBranch->body);
				}
				else if (auto* b = dynamic_cast<awst::Block*>(stmt.get()))
					walkCoerce(b->body);
				else if (auto* wl = dynamic_cast<awst::WhileLoop*>(stmt.get()))
					if (wl->loopBody) walkCoerce(wl->loopBody->body);
			}
		};
		walkCoerce(method.body->body);
	}
}

void encodeChainDispatchReturn(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper& m_typeMapper)
{
	if (!method.arc4MethodConfig.has_value() || !method.body)
		return;
	auto const& returnParams = _func.returnParameters();
	if (returnParams.empty())
		return;

	// The chain threaded NATIVE values (biguint) through its subs and Pass 4
	// already sign-extended signed elements in the body; here we ARC4-encode the
	// OUTER dispatch return to the shared wire plan. (signed→arc4.uint256,
	// unsigned biguint→arc4.uintN; everything else is already its wire type.)
	auto const plan = computeReturnPlan(_func, method.returnType, m_typeMapper);
	bool anyEncode = false;
	for (auto const& p: plan) if (p.encoded) { anyEncode = true; break; }
	if (!anyEncode)
		return;

	// The outer dispatch body ends with the single threaded return; encode it.
	forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
		if (!ret.value)
			return;
		auto loc = ret.value->sourceLocation;
		if (plan.size() == 1)
		{
			if (ret.value->wtype == awst::WType::biguintType())
				ret.value = awst::makeARC4Encode(std::move(ret.value), plan[0].wireType, loc);
		}
		else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret.value.get()))
		{
			std::vector<awst::WType const*> wireTypes;
			for (size_t i = 0; i < tuple->items.size() && i < plan.size(); ++i)
			{
				if (plan[i].encoded
					&& tuple->items[i]->wtype == awst::WType::biguintType())
					tuple->items[i] = awst::makeARC4Encode(
						std::move(tuple->items[i]), plan[i].wireType, loc);
			}
			for (auto const& p: plan)
				wireTypes.push_back(p.wireType);
			tuple->wtype = m_typeMapper.createType<awst::WTuple>(std::move(wireTypes));
		}
	});

	if (plan.size() == 1)
		method.returnType = plan[0].wireType;
	else
	{
		std::vector<awst::WType const*> wireTypes;
		for (auto const& p: plan)
			wireTypes.push_back(p.wireType);
		method.returnType = m_typeMapper.createType<awst::WTuple>(std::move(wireTypes));
	}
}

} // namespace puyasol::builder
