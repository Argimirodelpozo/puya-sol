/// @file SolExternalCall.cpp
/// External interface/contract calls via inner app transactions.
/// Migrated from FunctionCallBuilder.cpp lines 3662-4084.

#include "builder/sol-ast/calls/SolExternalCall.h"
#include "builder/itxn/InnerCallHandlers.h"
#include "builder/sol-types/TypeMapper.h"
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
	if (!_t)
		return false;
	if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(_t))
		_t = &udvt->underlyingType();
	auto const* intT = dynamic_cast<IntegerType const*>(_t);
	return intT && intT->isSigned();
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

		auto tuple = awst::makeTupleExpression(_returnType, m_loc);

		// Per-field Solidity types — a signed int field is 32 bytes on the wire (callee names it
		// uint256), so its uint64-backed WType needs a 32→low-8 decode, not a bare 8-byte btoi.
		auto const* solTuple = dynamic_cast<TupleType const*>(_solReturnType);

		int offset = 0;
		for (size_t i = 0; i < tupleType->types().size(); ++i)
		{
			auto const* fieldType = tupleType->types()[i];
			Type const* solField = (solTuple && i < solTuple->components().size())
				? solTuple->components()[i] : nullptr;
			bool signedNarrow = (fieldType == awst::WType::uint64Type()) && isSignedIntReturn(solField);
			int fieldSize = 0;

			if (fieldType == awst::WType::biguintType())
			{
				// The callee encodes an UNSIGNED uintN (64<N<=256) at its NATURAL N/8-byte ARC4 width
				// (uint128 -> 16B), not 32. Signed (int128/256) and uint256 are 32B. Match it so the
				// tuple offsets line up; makeAsBiguint below is length-agnostic.
				fieldSize = 32;
				if (!isSignedIntReturn(solField))
				{
					Type const* st = solField;
					if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(st))
						st = &udvt->underlyingType();
					if (auto const* it = dynamic_cast<IntegerType const*>(st))
						fieldSize = static_cast<int>(it->numBits() / 8);
				}
			}
			else if (fieldType == awst::WType::uint64Type())
				fieldSize = signedNarrow ? 32 : 8;   // signed int8/16/32/64 arrive as a 32-byte uint256
			else if (fieldType == awst::WType::boolType())
				fieldSize = 1;
			else if (fieldType == awst::WType::accountType())
				fieldSize = 32;
			else if (auto const* bwt = dynamic_cast<awst::BytesWType const*>(fieldType))
			{
				if (bwt->length().has_value())
					fieldSize = static_cast<int>(bwt->length().value());
			}

			if (fieldSize == 0)
			{
				tuple->items.push_back(singleBytes);
				break;
			}

			auto extract = awst::makeExtract3(singleBytes, awst::makeIntegerConstant(offset, m_loc), awst::makeIntegerConstant(fieldSize, m_loc), m_loc);
			std::shared_ptr<awst::Expression> decoded;
			if (fieldType == awst::WType::biguintType())
			{
				auto cast = awst::makeAsBiguint(std::move(extract), m_loc);
				decoded = std::move(cast);
			}
			else if (fieldType == awst::WType::uint64Type())
			{
				// signedNarrow: extract is the 32-byte uint256; its low 8 bytes are the canonical
				// uint64-backed two's-complement form. Unsigned uint64: extract is already 8 bytes.
				if (signedNarrow)
				{
					auto low8 = awst::makeExtract(std::move(extract), 24, 8, m_loc);
					decoded = awst::makeBtoi(std::move(low8), m_loc);
				}
				else
					decoded = awst::makeBtoi(std::move(extract), m_loc);
			}
			else if (fieldType == awst::WType::boolType())
			{
				auto getbit = awst::makeGetbit(
					std::move(extract), awst::makeZero(m_loc), m_loc);

				auto cmp = awst::makeNumericCompare(std::move(getbit), awst::NumericComparison::Ne, awst::makeIntegerConstant("0", m_loc), m_loc);
				decoded = std::move(cmp);
			}
			else if (fieldType == awst::WType::accountType())
			{
				auto cast = awst::makeAsAccount(std::move(extract), m_loc);
				decoded = std::move(cast);
			}
			else
			{
				decoded = std::move(extract);
			}

			tuple->items.push_back(std::move(decoded));
			offset += fieldSize;
		}

		std::vector<awst::WType const*> itemTypes;
		for (auto const& item: tuple->items)
			itemTypes.push_back(item->wtype);
		tuple->wtype = m_ctx.typeMapper.createType<awst::WTuple>(std::move(itemTypes), std::nullopt);
		return tuple;
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
