#include "builder/contract/ReturnRewriter.h"
#include "awst/NameGen.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/TypeMapper.h"

#include <boost/multiprecision/cpp_int.hpp>

#include <functional>
#include <set>
#include <string>

namespace puyasol::builder
{

void forEachReturnStatement(
	std::vector<std::shared_ptr<awst::Statement>>& _stmts,
	std::function<void(awst::ReturnStatement&)> const& _fn)
{
	for (auto& stmt: _stmts)
	{
		if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
			_fn(*ret);
		else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
		{
			if (ifElse->ifBranch) forEachReturnStatement(ifElse->ifBranch->body, _fn);
			if (ifElse->elseBranch) forEachReturnStatement(ifElse->elseBranch->body, _fn);
		}
		else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
			forEachReturnStatement(block->body, _fn);
		else if (auto* loop = dynamic_cast<awst::WhileLoop*>(stmt.get()))
		{
			if (loop->loopBody) forEachReturnStatement(loop->loopBody->body, _fn);
		}
	}
}

namespace
{

/// Per-return-element wire plan — THE single source of the "what ABI type does this
/// return element become" decision (see the SPEC below). Only biguint-backed elements
/// change type: a SIGNED biguint (sign-extended two's complement) publishes as
/// arc4.uint256, an UNSIGNED biguint as arc4.uintN(declared width). Everything else
/// (native uint64 incl. sub-word-masked unsigned, bool, address, bytesN, dynamic
/// bytes/string, arc4 aggregates) is already its own wire type.
struct ElemPlan
{
	awst::WType const* nativeType = nullptr;   // element type in method.returnType (post FunctionBuilder promotion)
	awst::WType const* wireType = nullptr;     // ABI wire type
	bool isSigned = false;                     // needs signExtendToUint256 prep before encode
	unsigned bits = 0;                         // declared width (wireType width + asm mod-wrap)
	bool encoded = false;                      // wireType != nativeType (a biguint element)
};

/// Compute the wire plan for every return element. Reads element native types from
/// `_returnType` (the promoted method.returnType: signed sub-64 and >64-bit ints are
/// already biguint there) — NOT a fresh map() of the Solidity type, which would give
/// int64→uint64 and miss the promotion. Solidity types supply signedness + declared bits.
std::vector<ElemPlan> computeReturnPlan(
	solidity::frontend::FunctionDefinition const& _func,
	awst::WType const* _returnType,
	TypeMapper& _tm)
{
	auto const& returnParams = _func.returnParameters();
	auto const* retTuple = (_returnType && _returnType->kind() == awst::WTypeKind::WTuple)
		? static_cast<awst::WTuple const*>(_returnType) : nullptr;
	std::vector<ElemPlan> plan;
	for (size_t i = 0; i < returnParams.size(); ++i)
	{
		ElemPlan p;
		p.nativeType = retTuple
			? (i < retTuple->types().size() ? retTuple->types()[i] : nullptr)
			: _returnType;
		p.wireType = p.nativeType;
		if (p.nativeType == awst::WType::biguintType())
		{
			auto si = SolIntType::fromSolOrEnum(returnParams[i]->type());
			p.isSigned = si && si->isSigned;
			p.bits = p.isSigned ? 256u : (si ? si->bits : 256u);
			p.wireType = _tm.createType<awst::ARC4UIntN>(static_cast<int>(p.bits));
			p.encoded = true;
		}
		plan.push_back(p);
	}
	return plan;
}

} // namespace

/// ── THE WIRE-RETURN-TYPE SPEC (D2 characterization, 2026-07-09) ──────────────
/// What the SIX passes below jointly compute, as a table (oracle fixture:
/// tests/WIP/return-wire-oracle/return_matrix.sol + oracle.txt — regenerate and
/// diff before/after ANY change to this file):
///   unsigned intN, N<=64           → native uint64            (pass 5 masks sub-word)
///   unsigned intN, 64<N<=256       → arc4.uintN               (pass 2 single / pass 3 tuple elem)
///   SIGNED intN, any width         → arc4.uint256             (pass 4; sign-extended two's complement)
///   bool / address / byte[N]       → native                   (untouched)
///   dynamic bytes / string         → native bytes/string      (untouched)
///   T[] (ReferenceArray)           → arc4 array               (pass 1)
///   tuple (static OR dynamic elem) → per-element by the rules above. Pass 3 wraps
///                                    biguint elements in ANY tuple (the old allStatic
///                                    guard that left (uint128,bytes) unwrapped is gone);
///                                    pass 4 handles signed elements.
///   MODIFIER'D fn (chain-lowered)  → the CHAIN threads NATIVE (biguint) values through
///                                    its subs; encodeChainDispatchReturn (called AFTER
///                                    buildModifierChain) encodes the OUTER dispatch return
///                                    to the wire type — signed→arc4.uint256, unsigned
///                                    biguint→arc4.uintN. buildModifierChain threads the
///                                    types method.returnType declares (NOT a fresh map(),
///                                    which gives int64→uint64 and mismatches the promoted
///                                    biguint body → "Tuple type mismatch").
///   asm-bodied biguint return      → arc4.uintN with `% 2^N` wrap (Yul is unchecked)
///
/// The per-element WIRE TYPE (which arc4.uintN a biguint element becomes) is computed
/// ONCE by computeReturnPlan and read by passes 2/3/4 AND encodeChainDispatchReturn — the
/// "pass 4 re-derives pass 3's biguint→uintN" copy-paste is gone (fable-review-2 D2). What
/// remains per-pass is the VALUE transform + the return-expression SHAPE walk: pass 2 scalar
/// biguint, pass 3 unsigned tuple (literal/ternary/opaque-spill), pass 4 signed (sign-extend
/// + the signed-tuple shapes), pass 5 sub-word mask, pass 6 named-return-var coercion. The
/// walks keep two intentional quirks that are output-load-bearing (a byte-identity gate holds
/// them): the opaque-tuple spill temp is `__ret_tmp_N` in the unsigned path (pass 3) but
/// `__rettuple_N` in the signed path (pass 4), and ternary-of-tuples is rewritten only in the
/// unsigned path. Merging the walks would change those names — deferred, low value.
void rewriteARC4Returns(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper& m_typeMapper,
	std::vector<SignedReturnInfo> const& signedReturns,
	std::vector<UnsignedMaskInfo> const& unsignedMasks,
	bool funcHasInlineAssembly)
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

	// THE per-element wire plan (shared with encodeChainDispatchReturn). Passes 2/3/4
	// below no longer each re-derive "biguint → which arc4.uintN" — they read it here.
	// Computed after Pass 1 so a ReferenceArray element already carries its arc4 type.
	auto const plan = computeReturnPlan(_func, method.returnType, m_typeMapper);

	// Assembly bodies are UNCHECKED (EVM Yul wraps mod 2^256); AVM biguint
	// does NOT wrap. Wrap (val % 2^N) before ARC4Encode for asm functions
	// to match EVM semantics. Non-asm leaves bare ARC4Encode so overflow REVERTS.
	auto pow2Str = [](unsigned bits) -> std::string {
		boost::multiprecision::cpp_int v = 1;
		v <<= bits;
		return v.str();
	};
	auto encodeRet = [&](std::shared_ptr<awst::Expression> val, unsigned bits,
		awst::WType const* arc4Ty, awst::SourceLocation const& loc)
		-> std::shared_ptr<awst::Expression>
	{
		if (funcHasInlineAssembly)
		{
			auto mod = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Mod,
				awst::makeIntegerConstant(pow2Str(bits), loc, awst::WType::biguintType()), loc);
			return awst::makeARC4Encode(std::move(mod), arc4Ty, loc);
		}
		return awst::makeARC4Encode(std::move(val), arc4Ty, loc);
	};

	// Pass 2: biguint returns → ARC4Encode(ARC4UIntN(N)); skipped for signed + modifier fns.
	if (method.arc4MethodConfig.has_value() && method.returnType == awst::WType::biguintType()
		&& signedReturns.empty() && _func.modifiers().empty())
	{
		// Wire type + declared width from the shared plan (single biguint element).
		unsigned retBits = plan[0].bits;
		auto const* arc4RetType = plan[0].wireType;

		forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
			if (ret.value && ret.value->wtype == awst::WType::biguintType())
			{
				auto loc = ret.value->sourceLocation;
				ret.value = encodeRet(std::move(ret.value), retBits, arc4RetType, loc);
			}
		});
		method.returnType = arc4RetType;
	}

	// Pass 3: tuple returns with biguint elements → per-element ARC4Encode.
	if (method.arc4MethodConfig.has_value() && method.returnType
		&& method.returnType->kind() == awst::WTypeKind::WTuple
		&& signedReturns.empty() && _func.modifiers().empty())
	{
		auto const* tupleType = static_cast<awst::WTuple const*>(method.returnType);
		// A biguint element in a tuple must be re-typed to its natural arc4.uintN —
		// else puya names it "uint512" → cross-contract selector mismatch → revert
		// (fuzzer: `(bytes4,uint128)` reverted unconditionally; then the oracle showed
		// the same for tuples with a DYNAMIC element, `(uint128,bytes)` — the old
		// allStatic guard excluded those, leaving the biguint unwrapped). Every other
		// element (uint64/bool/address/bytesN/dynamic bytes/string/arrays) stays
		// native; puya encodes mixed native+arc4 tuples fine.
		bool hasBiguintElement = false;
		for (auto const& p : plan)
			if (p.encoded) { hasBiguintElement = true; break; }

		if (hasBiguintElement)
		{
			// Per-element wire types from the shared plan (biguint → arc4.uintN, else native).
			std::vector<awst::WType const*> arc4Types;
			for (auto const& p : plan)
				arc4Types.push_back(p.wireType);

			auto wrapTupleItems = [&](awst::TupleExpression* tuple)
			{
				if (!tuple) return;
				for (size_t i = 0; i < tuple->items.size() && i < arc4Types.size(); ++i)
				{
					if (tuple->items[i]->wtype == awst::WType::biguintType()
						&& arc4Types[i]->kind() == awst::WTypeKind::ARC4UIntN)
					{
						unsigned bits = static_cast<unsigned>(
							static_cast<awst::ARC4UIntN const*>(arc4Types[i])->n());
						auto loc = tuple->items[i]->sourceLocation;
						tuple->items[i] = encodeRet(std::move(tuple->items[i]), bits, arc4Types[i], loc);
					}
				}
				tuple->wtype = new awst::WTuple(
					std::vector<awst::WType const*>(arc4Types));
			};

			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> wrapTupleReturns;
			wrapTupleReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
			{
				for (size_t si = 0; si < stmts.size(); ++si)
				{
					auto& stmt = stmts[si];
					if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
					{
						if (!ret->value) continue;
						if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret->value.get()))
							wrapTupleItems(tuple);
						else if (auto* cond = dynamic_cast<awst::ConditionalExpression*>(ret->value.get()))
						{
							wrapTupleItems(dynamic_cast<awst::TupleExpression*>(cond->trueExpr.get()));
							wrapTupleItems(dynamic_cast<awst::TupleExpression*>(cond->falseExpr.get()));
							cond->wtype = new awst::WTuple(
								std::vector<awst::WType const*>(arc4Types));
						}
						else if (ret->value->wtype
							&& ret->value->wtype->kind() == awst::WTypeKind::WTuple)
						{
							// Non-literal tuple (e.g. `return fu()`): spill to local,
							// then rebuild as TupleExpression of ARC4-encoded items.
							auto const* subTupleType = static_cast<awst::WTuple const*>(ret->value->wtype);
							bool needsWrap = false;
							for (auto const* t : subTupleType->types())
								if (t == awst::WType::biguintType()) { needsWrap = true; break; }
							if (!needsWrap) continue;

							std::string tmpName = "__ret_tmp_" + std::to_string(awst::NameGen::next("ReturnRewriter.retTmpCounter"));
							auto tmpVar = awst::makeVarExpression(tmpName, ret->value->wtype, ret->sourceLocation);

							auto assign = awst::makeAssignmentStatement(tmpVar, std::move(ret->value), ret->sourceLocation);

							auto newTuple = awst::makeTupleExpression(nullptr, assign->sourceLocation);
							for (size_t i = 0; i < arc4Types.size() && i < subTupleType->types().size(); ++i)
							{
								auto item = awst::makeTupleItem(tmpVar, static_cast<int>(i), subTupleType->types()[i], assign->sourceLocation);
								if (subTupleType->types()[i] == awst::WType::biguintType()
									&& arc4Types[i]->kind() == awst::WTypeKind::ARC4UIntN)
								{
									unsigned bits = static_cast<unsigned>(
										static_cast<awst::ARC4UIntN const*>(arc4Types[i])->n());
									newTuple->items.push_back(
										encodeRet(std::move(item), bits, arc4Types[i], assign->sourceLocation));
								}
								else
									newTuple->items.push_back(std::move(item));
							}
							newTuple->wtype = new awst::WTuple(
								std::vector<awst::WType const*>(arc4Types));
							ret->value = std::move(newTuple);

							stmts.insert(stmts.begin() + si, std::move(assign));
							++si; // skip the newly-inserted assign
						}
					}
					else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
					{
						if (ifElse->ifBranch) wrapTupleReturns(ifElse->ifBranch->body);
						if (ifElse->elseBranch) wrapTupleReturns(ifElse->elseBranch->body);
					}
					else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
						wrapTupleReturns(block->body);
					else if (auto* loop = dynamic_cast<awst::WhileLoop*>(stmt.get()))
						if (loop->loopBody) wrapTupleReturns(loop->loopBody->body);
				}
			};
			wrapTupleReturns(method.body->body);
			method.returnType = new awst::WTuple(std::vector<awst::WType const*>(arc4Types));
		}
	}

	// Pass 4: signed returns → signExtendToUint256; wrap in ARC4UIntN(256)
	// so ABI output is uint256 (32 bytes) not puya's default biguint→uint512.
	// For CHAIN-LOWERED (modifier'd) functions only the VALUE transform applies here:
	// the chain threads native return values through its subs, so per-item ARC4Encode
	// and the tuple retype would mismatch the subs' native slots ("Tuple type
	// mismatch"); encodeChainDispatchReturn encodes the OUTER dispatch return instead.
	bool const chainLowered = !_func.modifiers().empty();
	if (!signedReturns.empty() && method.arc4MethodConfig.has_value())
	{
		// All signed returns are wrapped to 256 bits by signExtendToUint256,
		// so the ABI element is uint256 in every case.
		auto const* arc4SignedType =
			m_typeMapper.createType<awst::ARC4UIntN>(256);

		auto wrapArc4 = [&](std::shared_ptr<awst::Expression> val,
			awst::SourceLocation const& loc) -> std::shared_ptr<awst::Expression> {
			if (val->wtype != awst::WType::biguintType())
				return val;
			auto encode = awst::makeARC4Encode(std::move(val), arc4SignedType, loc);
			return encode;
		};

		// For a signed-containing TUPLE return, Pass 2/3 are skipped, so the ARC4 element types are
		// never set — the signed (and any unsigned biguint) elements stay bare biguint and puya names
		// them "uint512", disagreeing with a caller's intSelectorReturnName (uint256 / uintN) → router
		// selector mismatch on cross-contract calls. Retype: signed → uint256; unsigned biguint →
		// uintN(declared); uint64/bool unchanged. (Found by the cross-contract differential fuzzer.)
		std::set<size_t> signedIdxSet;
		for (auto const& sr: signedReturns) signedIdxSet.insert(sr.index);
		awst::WType const* signedTupleRetType = nullptr;
		std::vector<awst::WType const*> signedTupleElems;   // for fresh `new WTuple` per assignment site
		if (!chainLowered && returnParams.size() > 1 && method.returnType
			&& method.returnType->kind() == awst::WTypeKind::WTuple)
		{
			// Per-element wire types from the shared plan (signed → arc4.uint256, unsigned
			// biguint → arc4.uintN, else native) — the same table Pass 3 uses, no longer
			// re-derived here ("Pass 4 re-does Pass 3" is gone).
			for (auto const& p : plan)
				signedTupleElems.push_back(p.wireType);
			signedTupleRetType = new awst::WTuple(
				std::vector<awst::WType const*>(signedTupleElems));   // mirror Pass 3's raw new
		}

		bool wrapSingleReturn = (signedReturns.size() == 1
			&& signedReturns[0].index == 0
			&& returnParams.size() == 1
			&& method.returnType == awst::WType::biguintType()
			&& _func.modifiers().empty()
			&& !funcHasInlineAssembly);

		forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
			if (!ret.value) return;
			auto srcLoc = ret.value->sourceLocation;

			if (signedReturns.size() == 1 && signedReturns[0].index == 0
				&& returnParams.size() == 1)
			{
				ret.value = TypeCoercion::signExtendToUint256(
					std::move(ret.value), signedReturns[0].bits, srcLoc);
				if (wrapSingleReturn)
					ret.value = wrapArc4(std::move(ret.value), srcLoc);
			}
			else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret.value.get()))
			{
				for (auto const& sr: signedReturns)
				{
					if (sr.index < tuple->items.size())
					{
						auto ext = TypeCoercion::signExtendToUint256(
							std::move(tuple->items[sr.index]), sr.bits, srcLoc);
						// Non-chain: ARC4-encode to uint256 to match signedTupleRetType.
						// Chain-lowered: keep the NATIVE sign-extended biguint (the chain
						// threads native slots; the outer dispatch return gets encoded).
						if (!chainLowered)
							ext = awst::makeARC4Encode(std::move(ext), arc4SignedType, srcLoc);
						tuple->items[sr.index] = std::move(ext);
					}
				}
				if (signedTupleRetType)
				{
					// Wrap any unsigned biguint elements too (Pass 3 was skipped) so they ARC4-encode
					// to uintN rather than staying bare biguint (→ uint512).
					auto const* newT = static_cast<awst::WTuple const*>(signedTupleRetType);
					for (size_t i = 0; i < tuple->items.size() && i < newT->types().size(); ++i)
						if (!signedIdxSet.count(i)
							&& tuple->items[i]->wtype == awst::WType::biguintType()
							&& newT->types()[i]->kind() == awst::WTypeKind::ARC4UIntN)
						{
							unsigned bits = static_cast<unsigned>(
								static_cast<awst::ARC4UIntN const*>(newT->types()[i])->n());
							tuple->items[i] = encodeRet(
								std::move(tuple->items[i]), bits, newT->types()[i], srcLoc);
						}
					tuple->wtype = new awst::WTuple(std::vector<awst::WType const*>(signedTupleElems));
				}
				else
					tuple->wtype = method.returnType;
			}
			else if (returnParams.size() > 1)
			{
				// Opaque tuple-producing return value (abi.decode, an internal call returning a tuple)
				// is NOT a tuple literal, so the per-item widening above can't reach its elements. Bind
				// it to a temp (eval once: the value may be a side-effecting call), then rebuild as a
				// tuple whose signed sub-64 elements are sign-extended to biguint to match the declared
				// ABI return type. Without this the decoded uint64 element mismatched the biguint return
				// slot and puya rejected the subroutine (invalid return type).
				if (auto const* nativeTuple = dynamic_cast<awst::WTuple const*>(ret.value->wtype))
				{
					auto const* tupleWType = ret.value->wtype;
					std::string tn = "__rettuple_" + std::to_string((awst::NameGen::next("ReturnRewriter.s_retTupleId") + 1));
					auto bind = awst::makeAssignmentExpression(
						awst::makeVarExpression(tn, tupleWType, srcLoc),
						std::move(ret.value), srcLoc, tupleWType);
					auto rebuilt = awst::makeTupleExpression(method.returnType, srcLoc);
					for (size_t i = 0; i < nativeTuple->types().size(); ++i)
					{
						std::shared_ptr<awst::Expression> item = awst::makeTupleItem(
							awst::makeVarExpression(tn, tupleWType, srcLoc),
							static_cast<int>(i), nativeTuple->types()[i], srcLoc);
						bool didSigned = false;
						for (auto const& sr: signedReturns)
							if (sr.index == i)
							{
								{ auto ext = TypeCoercion::signExtendToUint256(
									std::move(item), sr.bits, srcLoc);
								item = awst::makeARC4Encode(std::move(ext), arc4SignedType, srcLoc); }
								didSigned = true;
								break;
							}
						// Unsigned biguint element: ARC4Encode to its retyped uintN slot too (else stays bare
						// biguint, mismatching signedTupleRetType -> Tuple type mismatch).
						if (!didSigned && signedTupleRetType && i < signedTupleElems.size()
							&& item->wtype == awst::WType::biguintType()
							&& signedTupleElems[i]->kind() == awst::WTypeKind::ARC4UIntN)
						{
							unsigned bits = static_cast<unsigned>(
								static_cast<awst::ARC4UIntN const*>(signedTupleElems[i])->n());
							item = encodeRet(std::move(item), bits, signedTupleElems[i], srcLoc);
						}
						rebuilt->items.push_back(std::move(item));
					}
					awst::WType const* rebuiltType = signedTupleRetType
						? new awst::WTuple(std::vector<awst::WType const*>(signedTupleElems)) : method.returnType;
					rebuilt->wtype = rebuiltType;
					auto comma = awst::makeCommaExpression(rebuiltType, srcLoc);
					comma->expressions.push_back(std::move(bind));
					comma->expressions.push_back(std::move(rebuilt));
					ret.value = std::move(comma);
				}
			}
		});

		if (wrapSingleReturn)
			method.returnType = arc4SignedType;
		else if (signedTupleRetType)
			method.returnType = new awst::WTuple(std::vector<awst::WType const*>(signedTupleElems));
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
			tuple->wtype = new awst::WTuple(std::move(wireTypes));   // fresh instance (Pass 3/4 convention)
		}
	});

	if (plan.size() == 1)
		method.returnType = plan[0].wireType;
	else
	{
		std::vector<awst::WType const*> wireTypes;
		for (auto const& p: plan)
			wireTypes.push_back(p.wireType);
		method.returnType = new awst::WTuple(std::move(wireTypes));
	}
}

} // namespace puyasol::builder
