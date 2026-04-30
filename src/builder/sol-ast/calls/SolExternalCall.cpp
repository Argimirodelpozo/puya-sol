/// @file SolExternalCall.cpp
/// External interface/contract calls via inner app transactions.
/// Migrated from FunctionCallBuilder.cpp lines 3662-4084.

#include "builder/sol-ast/calls/SolExternalCall.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

static constexpr int TxnTypeAppl = 6;


std::string SolExternalCall::buildMethodSelector(MemberAccess const& _memberAccess)
{
	auto solTypeToARC4Name = [this](Type const* _type) -> std::string {
		auto* rawType = m_ctx.typeMapper.map(_type);
		if (rawType == awst::WType::accountType())
			return "address";
		// Note: signed/unsigned int types both map to biguint (or uint64) on the
		// callee side, so puya emits a `uint{N}` selector regardless of Solidity
		// signedness. We must mirror that here or selectors won't match.
		// Fixed-size Solidity `bytesN` stays as BytesWType(length=N) on the
		// child side, which puya names `byte[N]`. Match that here rather than
		// routing through ARC4StaticArray (which would produce `uint8[N]`).
		if (rawType->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bw = static_cast<awst::BytesWType const*>(rawType);
			if (bw->length().has_value())
				return "byte[" + std::to_string(*bw->length()) + "]";
		}
		auto* arc4Type = m_ctx.typeMapper.mapToARC4Type(rawType);
		return builder::TypeCoercion::wtypeToABIName(arc4Type);
	};

	std::string selector = _memberAccess.memberName() + "(";
	auto const* extRefDecl = _memberAccess.annotation().referencedDeclaration;

	if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(extRefDecl))
	{
		bool first = true;
		for (auto const& param: funcDef->parameters())
		{
			if (!first) selector += ",";
			selector += solTypeToARC4Name(param->type());
			first = false;
		}
		selector += ")";
		if (funcDef->returnParameters().size() > 1)
		{
			selector += "(";
			bool firstRet = true;
			for (auto const& retParam: funcDef->returnParameters())
			{
				if (!firstRet) selector += ",";
				selector += solTypeToARC4Name(retParam->type());
				firstRet = false;
			}
			selector += ")";
		}
		else if (funcDef->returnParameters().size() == 1)
			selector += solTypeToARC4Name(funcDef->returnParameters()[0]->type());
		else
			selector += "void";
	}
	else if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extRefDecl))
	{
		selector += ")";
		selector += solTypeToARC4Name(varDecl->type());
	}
	else
		selector += ")void";

	return selector;
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
			// ARC4 byte[] encoding: uint16(length) ++ raw_bytes
			auto lenExpr = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), m_loc);
			lenExpr->stackArgs.push_back(_argExpr);

			auto itobLen = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
			itobLen->stackArgs.push_back(std::move(lenExpr));

			auto header = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), m_loc);
			header->immediates = {6, 2};
			header->stackArgs.push_back(std::move(itobLen));

			auto encoded = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), m_loc);
			encoded->stackArgs.push_back(std::move(header));
			encoded->stackArgs.push_back(std::move(_argExpr));
			return encoded;
		}
		return _argExpr;
	}
	else if (_argExpr->wtype == awst::WType::uint64Type())
	{
		// itob produces 8 bytes. If the target ABI param is wider (e.g.
		// uint256 = 32 bytes), left-pad with zeros so the callee's arc4
		// length check (len == N) holds.
		unsigned widthBytes = 8;
		if (_paramSolType)
		{
			if (auto const* intType = dynamic_cast<IntegerType const*>(_paramSolType))
				widthBytes = intType->numBits() / 8;
			else if (auto const* addr = dynamic_cast<AddressType const*>(_paramSolType))
				(void)addr, widthBytes = 20;
		}
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
		itob->stackArgs.push_back(std::move(_argExpr));
		if (widthBytes <= 8)
			return itob;
		// pad = bzero(widthBytes - 8)  ++  itob(value)
		auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), m_loc);
		zeros->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(widthBytes - 8), m_loc));
		auto padded = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), m_loc);
		padded->stackArgs.push_back(std::move(zeros));
		padded->stackArgs.push_back(std::move(itob));
		return padded;
	}
	else if (_argExpr->wtype == awst::WType::biguintType())
	{
		// biguint → 32 bytes, left-padded
		auto cast = awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), m_loc);

		auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), m_loc);
		zeros->stackArgs.push_back(awst::makeIntegerConstant("32", m_loc));

		auto padded = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), m_loc);
		padded->stackArgs.push_back(std::move(zeros));
		padded->stackArgs.push_back(std::move(cast));

		auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), m_loc);
		lenCall->stackArgs.push_back(padded);

		auto offset = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), m_loc);
		offset->stackArgs.push_back(std::move(lenCall));
		offset->stackArgs.push_back(awst::makeIntegerConstant("32", m_loc));

		auto extracted = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), m_loc);
		extracted->stackArgs.push_back(std::move(padded));
		extracted->stackArgs.push_back(std::move(offset));
		extracted->stackArgs.push_back(awst::makeIntegerConstant("32", m_loc));
		return extracted;
	}
	else if (_argExpr->wtype == awst::WType::boolType())
	{
		// bool → ARC4 bool = setbit(0x00, 0, boolValue)
		auto zeroByte = awst::makeBytesConstant({0x00}, m_loc);

		auto setbit = awst::makeIntrinsicCall("setbit", awst::WType::bytesType(), m_loc);
		setbit->stackArgs.push_back(std::move(zeroByte));
		setbit->stackArgs.push_back(awst::makeIntegerConstant("0", m_loc));
		setbit->stackArgs.push_back(std::move(_argExpr));
		return setbit;
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
			auto encode = std::make_shared<awst::ARC4Encode>();
			encode->sourceLocation = m_loc;
			encode->wtype = arc4ArrayType;
			encode->value = std::move(_argExpr);

			auto rcast = awst::makeReinterpretCast(std::move(encode), awst::WType::bytesType(), m_loc);
			return rcast;
		}
	}
	else if (_argExpr->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| _argExpr->wtype->kind() == awst::WTypeKind::ARC4DynamicArray
		|| _argExpr->wtype->kind() == awst::WTypeKind::ARC4Struct
		|| _argExpr->wtype->kind() == awst::WTypeKind::ARC4Tuple)
	{
		auto rcast = awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), m_loc);
		return rcast;
	}
	else
	{
		auto rcast = awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), m_loc);
		return rcast;
	}
}

std::shared_ptr<awst::Expression> SolExternalCall::addressToAppId(
	std::shared_ptr<awst::Expression> _addrExpr)
{
	if (_addrExpr->wtype == awst::WType::applicationType())
		return _addrExpr;

	// Special case: `this` (CurrentApplicationAddress) is a hash, not our
	// conventional \x00*24 + app_id format. Use CurrentApplicationID directly.
	if (auto const* intrinsic = dynamic_cast<awst::IntrinsicCall const*>(_addrExpr.get()))
	{
		if (intrinsic->opCode == "global" && !intrinsic->immediates.empty())
		{
			auto const* imm = std::get_if<std::string>(&intrinsic->immediates[0]);
			if (imm && *imm == "CurrentApplicationAddress")
			{
				auto appId = awst::makeIntrinsicCall("global", awst::WType::uint64Type(), m_loc);
				appId->immediates = {std::string("CurrentApplicationID")};

				auto cast = awst::makeReinterpretCast(std::move(appId), awst::WType::applicationType(), m_loc);
				return cast;
			}
		}
	}

	std::shared_ptr<awst::Expression> bytesExpr = std::move(_addrExpr);
	if (bytesExpr->wtype == awst::WType::accountType())
	{
		auto toBytes = awst::makeReinterpretCast(std::move(bytesExpr), awst::WType::bytesType(), m_loc);
		bytesExpr = std::move(toBytes);
	}

	// extract last 8 bytes from 32-byte address
	auto extract = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), m_loc);
	extract->immediates = {24, 8};
	extract->stackArgs.push_back(std::move(bytesExpr));

	auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), m_loc);
	btoi->stackArgs.push_back(std::move(extract));

	auto cast = awst::makeReinterpretCast(std::move(btoi), awst::WType::applicationType(), m_loc);
	return cast;
}

std::shared_ptr<awst::Expression> SolExternalCall::submitAndReturn(
	std::shared_ptr<awst::Expression> _create,
	awst::WType const* _returnType)
{
	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = m_loc;
	submit->wtype = &s_applTxnType;
	submit->itxns.push_back(std::move(_create));

	// For void returns
	if (!_returnType || _returnType == awst::WType::voidType())
		return submit;

	// Submit as pre-pending statement, then extract from LastLog
	auto submitStmt = awst::makeExpressionStatement(std::move(submit), m_loc);
	m_ctx.prePendingStatements.push_back(std::move(submitStmt));

	auto readLog = awst::makeIntrinsicCall("itxn", awst::WType::bytesType(), m_loc);
	readLog->immediates = {std::string("LastLog")};

	// Strip 4-byte ARC4 return prefix
	auto stripPrefix = std::make_shared<awst::IntrinsicCall>();
	stripPrefix->sourceLocation = m_loc;
	stripPrefix->opCode = "extract";
	stripPrefix->immediates = {4, 0};
	stripPrefix->wtype = awst::WType::bytesType();
	stripPrefix->stackArgs.push_back(std::move(readLog));

	if (_returnType == awst::WType::biguintType())
	{
		auto cast = awst::makeReinterpretCast(std::move(stripPrefix), awst::WType::biguintType(), m_loc);
		return cast;
	}
	else if (_returnType == awst::WType::uint64Type())
	{
		auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), m_loc);
		btoi->stackArgs.push_back(std::move(stripPrefix));
		return btoi;
	}
	else if (_returnType == awst::WType::boolType())
	{
		auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), m_loc);
		getbit->stackArgs.push_back(std::move(stripPrefix));
		getbit->stackArgs.push_back(awst::makeIntegerConstant("0", m_loc));

		auto cmp = awst::makeNumericCompare(std::move(getbit), awst::NumericComparison::Ne, awst::makeIntegerConstant("0", m_loc), m_loc);
		return cmp;
	}
	else if (_returnType == awst::WType::accountType())
	{
		auto cast = awst::makeReinterpretCast(std::move(stripPrefix), awst::WType::accountType(), m_loc);
		return cast;
	}

	// Tuple/struct returns
	if (auto const* tupleType = dynamic_cast<awst::WTuple const*>(_returnType))
	{
		auto singleBytes = std::make_shared<awst::SingleEvaluation>();
		singleBytes->sourceLocation = m_loc;
		singleBytes->wtype = awst::WType::bytesType();
		singleBytes->source = std::move(stripPrefix);
		singleBytes->id = 0;

		auto tuple = std::make_shared<awst::TupleExpression>();
		tuple->sourceLocation = m_loc;
		tuple->wtype = _returnType;

		int offset = 0;
		for (size_t i = 0; i < tupleType->types().size(); ++i)
		{
			auto const* fieldType = tupleType->types()[i];
			int fieldSize = 0;

			if (fieldType == awst::WType::biguintType())
				fieldSize = 32;
			else if (fieldType == awst::WType::uint64Type())
				fieldSize = 8;
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

			auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), m_loc);
			extract->stackArgs.push_back(singleBytes);
			extract->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(offset), m_loc));
			extract->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(fieldSize), m_loc));

			std::shared_ptr<awst::Expression> decoded;
			if (fieldType == awst::WType::biguintType())
			{
				auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), m_loc);
				decoded = std::move(cast);
			}
			else if (fieldType == awst::WType::uint64Type())
			{
				auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), m_loc);
				btoi->stackArgs.push_back(std::move(extract));
				decoded = std::move(btoi);
			}
			else if (fieldType == awst::WType::boolType())
			{
				auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), m_loc);
				getbit->stackArgs.push_back(std::move(extract));
				getbit->stackArgs.push_back(awst::makeIntegerConstant("0", m_loc));

				auto cmp = awst::makeNumericCompare(std::move(getbit), awst::NumericComparison::Ne, awst::makeIntegerConstant("0", m_loc), m_loc);
				decoded = std::move(cmp);
			}
			else if (fieldType == awst::WType::accountType())
			{
				auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::accountType(), m_loc);
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
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = m_loc;
		vc->wtype = awst::WType::voidType();
		return vc;
	}

	// Optimise `(new C()).stateVar()` (or .immutable()) to the compile-time
	// initial value of the state variable. Our `new C()` stub only deploys
	// a tiny approval program that doesn't actually include the contract's
	// state initialisers, so any call on the resulting app returns zero
	// bytes (no log) and trips the itxn LastLog extraction. If we can see
	// the member is a public state/immutable var with a literal initialiser,
	// fold the call to that literal directly.
	{
		// Parentheses in the source become 1-element TupleExpressions in
		// the AST (e.g. `(new C())` is Tuple(FunctionCall(NewExpression))),
		// so peel them before looking for `new C()`. Without this the
		// fold never fires for the common parenthesised form.
		Expression const* base = &memberAccess->expression();
		while (auto const* tup = dynamic_cast<TupleExpression const*>(base))
		{
			if (tup->components().size() == 1 && tup->components()[0])
				base = tup->components()[0].get();
			else
				break;
		}
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
			if (auto const* scope = funcDef->scope())
			{
				if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(scope))
				{
					if (contractDef->isLibrary())
					{
						Logger::instance().error(
							"delegatecall to public library function '" + contractDef->name()
							+ "." + funcDef->name() + "' is not supported on AVM. "
							"Use internal library functions instead.", m_loc);
					}
				}
			}
		}
	}

	auto baseTranslated = buildExpr(memberAccess->expression());

	// Build method selector
	std::string methodSelector = buildMethodSelector(*memberAccess);
	auto methodConst = std::make_shared<awst::MethodConstant>();
	methodConst->sourceLocation = m_loc;
	methodConst->wtype = awst::WType::bytesType();
	methodConst->value = methodSelector;

	// Build ApplicationArgs tuple
	auto argsTuple = std::make_shared<awst::TupleExpression>();
	argsTuple->sourceLocation = m_loc;
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

				auto cast = awst::makeReinterpretCast(std::move(elem), awst::WType::bytesType(), m_loc);

				auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), m_loc);
				zeros->stackArgs.push_back(awst::makeIntegerConstant("32", m_loc));

				auto padded = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), m_loc);
				padded->stackArgs.push_back(std::move(zeros));
				padded->stackArgs.push_back(std::move(cast));

				auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), m_loc);
				lenCall->stackArgs.push_back(padded);

				auto off = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), m_loc);
				off->stackArgs.push_back(std::move(lenCall));
				off->stackArgs.push_back(awst::makeIntegerConstant("32", m_loc));

				auto extracted = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), m_loc);
				extracted->stackArgs.push_back(std::move(padded));
				extracted->stackArgs.push_back(std::move(off));
				extracted->stackArgs.push_back(awst::makeIntegerConstant("32", m_loc));

				if (!acc)
					acc = std::move(extracted);
				else
				{
					auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), m_loc);
					cat->stackArgs.push_back(std::move(acc));
					cat->stackArgs.push_back(std::move(extracted));
					acc = std::move(cat);
				}
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
	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = m_loc;
	create->wtype = &s_applFieldsType;
	create->fields["TypeEnum"] = awst::makeIntegerConstant(std::to_string(TxnTypeAppl), m_loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", m_loc);
	create->fields["ApplicationID"] = std::move(appId);
	create->fields["OnCompletion"] = awst::makeIntegerConstant("0", m_loc);
	create->fields["ApplicationArgs"] = std::move(argsTuple);

	auto* retType = m_ctx.typeMapper.map(m_call.annotation().type);
	return submitAndReturn(std::move(create), retType);
}

} // namespace puyasol::builder::sol_ast
