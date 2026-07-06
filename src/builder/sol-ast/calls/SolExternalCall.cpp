/// @file SolExternalCall.cpp
/// External interface/contract calls via inner app transactions.
/// Migrated from FunctionCallBuilder.cpp lines 3662-4084.

#include "builder/sol-ast/calls/SolExternalCall.h"
#include "builder/itxn/InnerCallHandlers.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTUtils.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

static constexpr int TxnTypeAppl = 6;

/// A SIGNED Solidity integer RETURN is encoded by the callee as a 32-byte uint256 (sign-extended
/// two's complement), regardless of width (see TypeCoercion::intSelectorReturnName). So a signed
/// int8/16/32/64 — whose WType is uint64Type — arrives as 32 bytes on the wire, NOT 8. The caller's
/// decode must account for that (extract the low 8 bytes, then btoi) instead of btoi-ing 32 bytes
/// ("btoi arg too long"). int128/int256 (biguint WType) already decode 32 bytes correctly.
static bool isSignedIntReturn(solidity::frontend::Type const* _t)
{
	auto it = builder::SolIntType::fromSol(_t);
	return it && it->isSigned;
}


std::string SolExternalCall::buildMethodSelector(MemberAccess const& _memberAccess)
{
	auto solTypeToARC4Name = [this](Type const* _type) -> std::string {
		// Integers: <=64-bit → "uint64", >64-bit → "uintN" (exact width), signedness
		// dropped. map()→biguint would collapse all >64-bit to "uint256" (wrong).
		if (auto name = builder::TypeCoercion::intSelectorName(_type))
			return *name;
		auto* rawType = m_ctx.typeMapper.map(_type);
		if (rawType == awst::WType::accountType())
			return "address";
		// A top-level enum maps to uint64Type on the child side, which puya publishes
		// as "uint64" (see solTypeToARC4Impl). nestedArc4Name would name it "uint8"
		// (its encodingType width) → selector mismatch → router err on a cross-contract
		// call returning/taking an enum. (Found by the tuple-return fuzzer.)
		if (dynamic_cast<EnumType const*>(_type))
			return "uint64";
		// Fixed-size Solidity `bytesN` stays as BytesWType(length=N) on the
		// child side, which puya names `byte[N]`. Match that here rather than
		// routing through ARC4StaticArray (which would produce `uint8[N]`).
		if (rawType->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bw = static_cast<awst::BytesWType const*>(rawType);
			if (bw->length().has_value())
				return "byte[" + std::to_string(*bw->length()) + "]";
		}
		// Aggregates (struct/array): use the canonical nested namer, which PRESERVES signedness for
		// elements (struct field int64 = "int64", not "uint64") to match the callee's published ABI /
		// puya's emitted signature. wtypeToABIName drops the ARC4UIntN sign alias → selector mismatch
		// → router err on cross-contract calls with signed struct/array elements. (Found by the fuzzer.)
		return eb::nestedArc4Name(m_ctx, _type);
	};
	// Signed int returns → "uint256" (full 256-bit two's complement);
	// non-int types identical to params.
	auto solTypeToARC4Ret = [&](Type const* _type) -> std::string {
		if (auto name = builder::TypeCoercion::intSelectorReturnName(_type))
			return *name;
		return solTypeToARC4Name(_type);
	};

	auto const* extRefDecl = _memberAccess.annotation().referencedDeclaration;
	std::vector<std::string> paramNames, retNames;
	if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(extRefDecl))
	{
		for (auto const& param: funcDef->parameters())
			paramNames.push_back(solTypeToARC4Name(param->type()));
		for (auto const& retParam: funcDef->returnParameters())
			retNames.push_back(solTypeToARC4Ret(retParam->type()));
	}
	else if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extRefDecl))
		retNames.push_back(solTypeToARC4Ret(varDecl->type()));
	// else: no params/returns -> "name()void"

	return builder::TypeCoercion::buildArc4Selector(_memberAccess.memberName(), paramNames, retNames);
}

std::shared_ptr<awst::Expression> SolExternalCall::encodeArgToBytes(
	std::shared_ptr<awst::Expression> _argExpr,
	Type const* _paramSolType)
{
	bool isDynamicBytes = false;
	if (_paramSolType)
	{
		auto cat = _paramSolType->category();
		isDynamicBytes = (cat == Type::Category::Array
			&& dynamic_cast<ArrayType const*>(_paramSolType)
			&& dynamic_cast<ArrayType const*>(_paramSolType)->isByteArrayOrString());
	}

	if (_argExpr->wtype == awst::WType::bytesType()
		|| _argExpr->wtype->kind() == awst::WTypeKind::Bytes)
	{
		if (isDynamicBytes)
		{
			// ARC4 byte[]: uint16(len)++raw. makeEvalOnce for side-effecting args.
			_argExpr = awst::makeEvalOnce(std::move(_argExpr), m_loc);
			auto lenExpr = awst::makeLen(_argExpr, m_loc);
			auto itobLen = awst::makeItob(std::move(lenExpr), m_loc);
			auto header = awst::makeExtract(std::move(itobLen), 6, 2, m_loc);

			return awst::makeConcat(std::move(header), std::move(_argExpr), m_loc);
		}
		return _argExpr;
	}
	else if (_argExpr->wtype == awst::WType::uint64Type())
	{
		// itob → 8 bytes; left-pad if param is wider (callee's arc4 len check).
		unsigned widthBytes = 8;
		if (_paramSolType)
		{
			if (auto const* intType = dynamic_cast<IntegerType const*>(_paramSolType))
				widthBytes = intType->numBits() / 8;
			else if (auto const* addr = dynamic_cast<AddressType const*>(_paramSolType))
				(void)addr, widthBytes = 20;
		}
		auto itob = awst::makeItob(std::move(_argExpr), m_loc);
		if (widthBytes <= 8)
			return itob;
		// pad = bzero(widthBytes - 8)  ++  itob(value)
		return awst::makeLeftPad(std::move(itob), widthBytes - 8, m_loc);
	}
	else if (_argExpr->wtype == awst::WType::biguintType())
	{
		// Encode to the param's exact ARC4 width (N/8 bytes); callee arc4 decode
		// asserts len==N/8, so a 32-byte arg reverts. makeARC4Encode trims to low
		// N/8 bytes. int256/uint256 stays 32 bytes.
		auto const* solT = _paramSolType;
		if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(solT))
			solT = &udvt->underlyingType();
		if (dynamic_cast<IntegerType const*>(solT))
		{
			auto* arc4Type = m_ctx.typeMapper.mapSolTypeToARC4(_paramSolType);
			auto enc = awst::makeARC4Encode(std::move(_argExpr), arc4Type, m_loc);
			return awst::makeAsBytes(std::move(enc), m_loc);
		}
		// Non-integer biguint (rare): keep the 32-byte left-pad.
		auto cast = awst::makeAsBytes(std::move(_argExpr), m_loc);
		return awst::makeLeftPadToN(std::move(cast), 32, m_loc);
	}
	else if (_argExpr->wtype == awst::WType::boolType())
	{
		// bool → ARC4 bool = setbit(0x00, 0, boolValue)
		return awst::makeSetbit(
			awst::makeBytesConstant({0x00}, m_loc),
			awst::makeZero(m_loc),
			std::move(_argExpr), m_loc);
	}
	else if (_argExpr->wtype->kind() == awst::WTypeKind::ReferenceArray)
	{
		// ReferenceArray → ARC4 encode
		auto* refArr = dynamic_cast<awst::ReferenceArray const*>(_argExpr->wtype);
		auto* elemType = refArr ? refArr->elementType() : nullptr;
		auto* arc4ElemType = elemType ? m_ctx.typeMapper.mapToARC4Type(elemType) : nullptr;

		awst::WType const* arc4ArrayType = nullptr;
		if (arc4ElemType && refArr && refArr->arraySize())
			arc4ArrayType = m_ctx.typeMapper.createType<awst::ARC4StaticArray>(
				arc4ElemType, *refArr->arraySize());
		else if (arc4ElemType)
			arc4ArrayType = m_ctx.typeMapper.createType<awst::ARC4DynamicArray>(arc4ElemType);

		if (arc4ArrayType)
		{
			auto encode = awst::makeARC4Encode(std::move(_argExpr), arc4ArrayType, m_loc);

			auto rcast = awst::makeAsBytes(std::move(encode), m_loc);
			return rcast;
		}
	}
	else if (_argExpr->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| _argExpr->wtype->kind() == awst::WTypeKind::ARC4DynamicArray
		|| _argExpr->wtype->kind() == awst::WTypeKind::ARC4Struct
		|| _argExpr->wtype->kind() == awst::WTypeKind::ARC4Tuple)
	{
		auto rcast = awst::makeAsBytes(std::move(_argExpr), m_loc);
		return rcast;
	}
	else
	{
		auto rcast = awst::makeAsBytes(std::move(_argExpr), m_loc);
		return rcast;
	}
}

std::shared_ptr<awst::Expression> SolExternalCall::addressToAppId(
	std::shared_ptr<awst::Expression> _addrExpr)
{
	if (_addrExpr->wtype == awst::WType::applicationType())
		return _addrExpr;

	// `this` (CurrentApplicationAddress) is a hash, not \x00*24+app_id;
	// use CurrentApplicationID directly.
	if (auto const* intrinsic = dynamic_cast<awst::IntrinsicCall const*>(_addrExpr.get()))
	{
		if (intrinsic->opCode == "global" && !intrinsic->immediates.empty())
		{
			auto const* imm = std::get_if<std::string>(&intrinsic->immediates[0]);
			if (imm && *imm == "CurrentApplicationAddress")
			{
				auto appId = awst::makeGlobal(std::string("CurrentApplicationID"), awst::WType::uint64Type(), m_loc);

				auto cast = awst::makeAsApplication(std::move(appId), m_loc);
				return cast;
			}
		}
	}

	std::shared_ptr<awst::Expression> bytesExpr = std::move(_addrExpr);
	if (bytesExpr->wtype == awst::WType::accountType())
	{
		auto toBytes = awst::makeAsBytes(std::move(bytesExpr), m_loc);
		bytesExpr = std::move(toBytes);
	}

	// low 8 bytes of the 32-byte address → app id
	auto btoi = awst::makeWord32ToUInt64(std::move(bytesExpr), m_loc);
	return awst::makeAsApplication(std::move(btoi), m_loc);
}

std::shared_ptr<awst::Expression> SolExternalCall::submitAndReturn(
	std::shared_ptr<awst::Expression> _create,
	awst::WType const* _returnType,
	solidity::frontend::Type const* _solReturnType)
{
	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);
	auto submit = awst::makeSubmitInnerTransaction(&s_applTxnType, m_loc);
	submit->itxns.push_back(std::move(_create));

	// For void returns
	if (!_returnType || _returnType == awst::WType::voidType())
		return submit;

	// Submit as pre-pending statement, then extract from LastLog
	auto submitStmt = awst::makeExpressionStatement(std::move(submit), m_loc);
	m_ctx.prePendingStatements.push_back(std::move(submitStmt));

	auto readLog = awst::makeItxn("LastLog", awst::WType::bytesType(), m_loc);

	// Strip 4-byte ARC4 return prefix
	auto stripPrefix = awst::makeExtract(std::move(readLog), 4, 0, m_loc);

	if (_returnType == awst::WType::biguintType())
	{
		auto cast = awst::makeAsBiguint(std::move(stripPrefix), m_loc);
		return cast;
	}
	else if (_returnType == awst::WType::uint64Type())
	{
		// Signed int8/16/32/64: callee sent a 32-byte uint256 (sign-extended TC). The low 8 bytes are
		// the canonical uint64-backed two's-complement form — extract them, then btoi. Unsigned uint64
		// is 8 bytes on the wire, so btoi directly.
		if (isSignedIntReturn(_solReturnType))
		{
			auto low8 = awst::makeExtract(std::move(stripPrefix), 24, 8, m_loc);
			return awst::makeBtoi(std::move(low8), m_loc);
		}
		return awst::makeBtoi(std::move(stripPrefix), m_loc);
	}
	else if (_returnType == awst::WType::boolType())
	{
		auto getbit = awst::makeGetbit(
			std::move(stripPrefix), awst::makeZero(m_loc), m_loc);

		auto cmp = awst::makeNumericCompare(std::move(getbit), awst::NumericComparison::Ne, awst::makeIntegerConstant("0", m_loc), m_loc);
		return cmp;
	}
	else if (_returnType == awst::WType::accountType())
	{
		auto cast = awst::makeAsAccount(std::move(stripPrefix), m_loc);
		return cast;
	}

	// Tuple/struct returns
	if (auto const* tupleType = dynamic_cast<awst::WTuple const*>(_returnType))
	{
		// Intentionally RAW makeSingleEvaluation, not makeEvalOnce: the fresh id is
		// IDENTITY-FORCING — without it two identical calls compare attrs-equal and
		// merge, so the second call's itxn never submits. The wrap must be
		// unconditional; makeEvalOnce's skip-leaf contract must never apply here.
		auto singleBytes = awst::makeSingleEvaluation(
			std::move(stripPrefix), awst::WType::bytesType(),
			awst::nextSingleEvalId(), m_loc);

		// Wire ARC4 tuple type: the callee (ReturnRewriter) ARC4-encodes the return
		// tuple, so the raw log bytes ARE an ARC4 tuple. Reinterpret to that type and
		// hand the head/tail/bool-packing/dynamic-field layout to puya's ARC4Decode
		// rather than walking byte offsets by hand. The one convention puya's generic
		// map doesn't capture is the signed-int wire width: a SIGNED intN return is
		// sign-extended to uint256 (32B) by ReturnRewriter Pass 4, regardless of width.
		// UNSIGNED biguints keep their NATURAL declared width (uint128 → 16B) in every
		// case — Pass 3 (all-unsigned) and Pass 4 (signed-containing, ReturnRewriter.cpp
		// line 272-281) both encode them at uintN, never widened. Build the wire element
		// types to match exactly.
		auto const* solTuple = dynamic_cast<TupleType const*>(_solReturnType);

		size_t const n = tupleType->types().size();
		std::vector<awst::WType const*> wireElems;   // arc4 element types (the wire)
		std::vector<awst::WType const*> decodeElems; // native decode target per element
		std::vector<bool> narrowIdx(n, false);       // signed-narrow slots to reconcile
		bool anyNarrow = false;
		wireElems.reserve(n);
		decodeElems.reserve(n);
		for (size_t i = 0; i < n; ++i)
		{
			auto const* nat = tupleType->types()[i];
			Type const* solField = (solTuple && i < solTuple->components().size())
				? solTuple->components()[i] : nullptr;
			auto si = solField ? builder::SolIntType::fromSolOrEnum(solField) : std::nullopt;
			awst::WType const* arc4 = nullptr;
			awst::WType const* dec = nat;   // default: puya decodes to the native return type
			if (si && isSignedIntReturn(solField))
			{
				// SIGNED intN: wire is a 32-byte sign-extended uint256 (Pass 4). puya's
				// ARC4Decode of arc4.uint256 yields a BIGUINT holding that 256-bit value;
				// int128/256 want biguint directly, but a signed int8..64 wants a 64-bit
				// native — decoding straight to uint64 REVERTS on negatives (2^256-|x| ≫
				// 2^64). So decode to biguint here and narrow to uint64 in the rebuild.
				arc4 = m_ctx.typeMapper.createType<awst::ARC4UIntN>(256);
				if (nat == awst::WType::uint64Type())
				{
					dec = awst::WType::biguintType();
					narrowIdx[i] = true;
					anyNarrow = true;
				}
			}
			else if (si)
			{
				// Unsigned: uint8..64 / enum → 8B (arc4.uint64); uint65..256 at its
				// natural declared width (uint128 → 16B, uint256 → 32B).
				if (si->bits <= 64)
					arc4 = m_ctx.typeMapper.createType<awst::ARC4UIntN>(64);
				else
					arc4 = m_ctx.typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(si->bits));
			}
			else
				arc4 = m_ctx.typeMapper.mapToARC4Type(nat);   // bool/address/bytesN/dynamic/nested

			wireElems.push_back(arc4);
			decodeElems.push_back(dec);
		}
		auto const* arc4TupleType =
			m_ctx.typeMapper.createType<awst::ARC4Tuple>(std::move(wireElems));

		// The native tuple puya's ARC4Decode produces — identical to _returnType except
		// signed-narrow slots are biguint (reconciled below).
		awst::WType const* decodeTarget = anyNarrow
			? m_ctx.typeMapper.createType<awst::WTuple>(decodeElems, std::nullopt)
			: _returnType;

		auto arc4Val = awst::makeReinterpretCast(std::move(singleBytes), arc4TupleType, m_loc);
		auto decoded = awst::makeARC4Decode(std::move(arc4Val), decodeTarget, m_loc);
		if (!anyNarrow)
			return decoded;

		// Rebuild to _returnType: index each element out of the decoded tuple and
		// narrow the signed-narrow biguint slots to their 64-bit two's-complement form
		// (implicitNumericCast biguint→uint64 = pad→low-8-bytes→btoi).
		auto decodedSE = awst::makeSingleEvaluation(
			std::move(decoded), decodeTarget, awst::nextSingleEvalId(), m_loc);
		auto result = awst::makeTupleExpression(_returnType, m_loc);
		for (size_t i = 0; i < n; ++i)
		{
			std::shared_ptr<awst::Expression> item = awst::makeTupleItem(
				decodedSE, static_cast<int>(i), decodeElems[i], m_loc);
			if (narrowIdx[i])
				item = builder::TypeCoercion::implicitNumericCast(
					std::move(item), awst::WType::uint64Type(), m_loc);
			result->items.push_back(std::move(item));
		}
		result->wtype = _returnType;
		return result;
	}

	// ARC4 aggregate return types — reinterpret the raw bytes
	if (_returnType
		&& (_returnType->kind() == awst::WTypeKind::ARC4DynamicArray
			|| _returnType->kind() == awst::WTypeKind::ARC4StaticArray
			|| _returnType->kind() == awst::WTypeKind::ARC4Struct
			|| _returnType->kind() == awst::WTypeKind::ARC4UIntN
			|| _returnType->kind() == awst::WTypeKind::ReferenceArray))
	{
		auto cast = awst::makeReinterpretCast(std::move(stripPrefix), _returnType, m_loc);
		return cast;
	}

	// Default: return raw bytes
	return stripPrefix;
}

std::shared_ptr<awst::Expression> SolExternalCall::toAwst()
{
	auto const& funcExpr = funcExpression();
	auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr);
	if (!memberAccess)
	{
		auto vc = awst::makeVoidConstant(m_loc);
		return vc;
	}

	// Fold `(new C()).stateVar()` to the literal initialiser. `new C()` stub
	// emits a minimal program; any call returns no log and trips itxn
	// LastLog extraction. Fold avoids the inner txn.
	{
		// `(new C())` → Tuple(FunctionCall(NewExpression)) in AST;
		// peel outer tuples or the fold never fires.
		Expression const* base = solidity::frontend::resolveOuterUnaryTuples(
			&memberAccess->expression());
		if (auto const* outerFuncCall = dynamic_cast<FunctionCall const*>(base))
		{
			if (auto const* newExpr = dynamic_cast<NewExpression const*>(&outerFuncCall->expression()))
			{
				auto const* refDecl = memberAccess->annotation().referencedDeclaration;
				auto const* varDecl = dynamic_cast<VariableDeclaration const*>(refDecl);
				if (newExpr && varDecl && varDecl->isStateVariable()
					&& varDecl->value()
					&& m_call.arguments().empty())
				{
					auto val = buildExpr(*varDecl->value());
					if (val)
					{
						auto const* retType = m_ctx.typeMapper.map(
							m_call.annotation().type);
						if (retType)
							val = builder::TypeCoercion::implicitNumericCast(
								std::move(val), retType, m_loc);
						Logger::instance().warning(
							"folded `(new " + (
								dynamic_cast<ContractType const*>(
									newExpr->typeName().annotation().type)
								? dynamic_cast<ContractType const*>(
									newExpr->typeName().annotation().type
								)->contractDefinition().name()
								: std::string("C"))
							+ ").stateVar()` to compile-time initial value", m_loc);
						return val;
					}
				}
			}
		}
	}

	// Detect delegatecall to library functions — not supported on AVM
	if (auto const* refDecl = memberAccess->annotation().referencedDeclaration)
	{
		if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
		{
			auto const* contractDef = funcDef->annotation().contract;
			if (contractDef && contractDef->isLibrary())
			{
				Logger::instance().error(
					"delegatecall to public library function '" + contractDef->name()
					+ "." + funcDef->name() + "' is not supported on AVM. "
					"Use internal library functions instead.", m_loc);
			}
		}
	}

	auto baseTranslated = buildExpr(memberAccess->expression());

	// Build method selector
	auto methodConst = awst::makeMethodConstant(
		buildMethodSelector(*memberAccess), awst::WType::bytesType(), m_loc);

	// Build ApplicationArgs tuple
	auto argsTuple = awst::makeTupleExpression(nullptr, m_loc);
	argsTuple->items.push_back(std::move(methodConst));

	// Get parameter types for encoding
	auto const* extRefDecl = memberAccess->annotation().referencedDeclaration;
	std::vector<Type const*> paramSolTypes;
	if (auto const* fd = dynamic_cast<FunctionDefinition const*>(extRefDecl))
		for (auto const& param: fd->parameters())
			paramSolTypes.push_back(param->type());

	// Add call arguments
	size_t argIdx = 0;
	for (auto const& arg: m_call.arguments())
	{
		Type const* paramType = (argIdx < paramSolTypes.size()) ? paramSolTypes[argIdx] : nullptr;
		++argIdx;

		// Handle inline array literals
		if (auto const* tupleExpr = dynamic_cast<TupleExpression const*>(arg.get());
			tupleExpr && tupleExpr->isInlineArray())
		{
			std::shared_ptr<awst::Expression> acc;
			for (auto const& comp: tupleExpr->components())
			{
				if (!comp) continue;
				auto elem = buildExpr(*comp);
				elem = builder::TypeCoercion::implicitNumericCast(
					std::move(elem), awst::WType::biguintType(), m_loc);

				auto cast = awst::makeAsBytes(std::move(elem), m_loc);
				auto extracted = awst::makeLeftPadToN(std::move(cast), 32, m_loc);

				if (!acc)
					acc = std::move(extracted);
				else
					acc = awst::makeConcat(std::move(acc), std::move(extracted), m_loc);
			}
			if (acc)
				argsTuple->items.push_back(std::move(acc));
			continue;
		}

		auto argExpr = buildExpr(*arg);
		argsTuple->items.push_back(encodeArgToBytes(std::move(argExpr), paramType));
	}

	// Build WTuple type for args
	std::vector<awst::WType const*> argTypes;
	for (auto const& item: argsTuple->items)
		argTypes.push_back(item->wtype);
	argsTuple->wtype = m_ctx.typeMapper.createType<awst::WTuple>(std::move(argTypes), std::nullopt);

	// Convert receiver to app ID
	auto appId = addressToAppId(std::move(baseTranslated));

	// Build inner app transaction
	static awst::WInnerTransactionFields s_applFieldsType(TxnTypeAppl);
	auto create = awst::makeCreateInnerTransaction(&s_applFieldsType, m_loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant(TxnTypeAppl, m_loc);
	create->fields["Fee"] = awst::makeZero(m_loc);
	create->fields["ApplicationID"] = std::move(appId);
	create->fields["OnCompletion"] = awst::makeZero(m_loc);
	create->fields["ApplicationArgs"] = std::move(argsTuple);

	auto* retType = m_ctx.typeMapper.map(m_call.annotation().type);
	return submitAndReturn(std::move(create), retType, m_call.annotation().type);
}

} // namespace puyasol::builder::sol_ast
