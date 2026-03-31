/// @file SolArrayMethod.cpp
/// array.push(val), array.push(), and array.pop().
/// Box-backed arrays read/write from box storage; memory arrays use AWST nodes directly.

#include "builder/sol-ast/calls/SolArrayMethod.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolArrayMethod::toAwst()
{
	auto const& funcExpr = funcExpression();
	auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr);
	if (!memberAccess)
		return nullptr;

	std::string memberName = memberAccess->memberName();
	auto const& baseExpr = memberAccess->expression();

	// Check if this is a box-stored dynamic array state variable
	if (auto const* ident = dynamic_cast<Identifier const*>(&baseExpr))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (varDecl->isStateVariable()
				&& builder::StorageMapper::shouldUseBoxStorage(*varDecl)
				&& dynamic_cast<ArrayType const*>(varDecl->type()))
			{
				return handleBoxArray(memberName, baseExpr, *varDecl);
			}
		}
	}

	return handleMemoryArray(memberName, baseExpr);
}

std::shared_ptr<awst::Expression> SolArrayMethod::handleBoxArray(
	std::string const& _memberName,
	Expression const& _baseExpr,
	VariableDeclaration const& _varDecl)
{
	auto const* solArrType = dynamic_cast<ArrayType const*>(_varDecl.type());
	auto* rawElemType = m_ctx.typeMapper.map(solArrType->baseType());
	auto* elemType = m_ctx.typeMapper.mapToARC4Type(rawElemType);
	auto* arrWType = m_ctx.typeMapper.createType<awst::ReferenceArray>(
		elemType, false, std::nullopt);

	auto const* ident = dynamic_cast<Identifier const*>(&_baseExpr);
	std::string arrayVarName = ident->name();

	// Build BoxValueExpression
	auto boxKey = std::make_shared<awst::BytesConstant>();
	boxKey->sourceLocation = m_loc;
	boxKey->wtype = awst::WType::boxKeyType();
	boxKey->encoding = awst::BytesEncoding::Utf8;
	boxKey->value = std::vector<uint8_t>(arrayVarName.begin(), arrayVarName.end());

	auto boxExpr = std::make_shared<awst::BoxValueExpression>();
	boxExpr->sourceLocation = m_loc;
	boxExpr->wtype = arrWType;
	boxExpr->key = boxKey;
	boxExpr->existsAssertionMessage = std::nullopt;

	// StateGet wrapper for reads (returns empty array if box missing)
	auto emptyArr = std::make_shared<awst::NewArray>();
	emptyArr->sourceLocation = m_loc;
	emptyArr->wtype = arrWType;

	auto stateGet = std::make_shared<awst::StateGet>();
	stateGet->sourceLocation = m_loc;
	stateGet->wtype = arrWType;
	stateGet->field = boxExpr;
	stateGet->defaultValue = emptyArr;

	std::shared_ptr<awst::Expression> writeExpr = boxExpr;

	if (_memberName == "push" && !m_call.arguments().empty())
	{
		auto val = buildExpr(*m_call.arguments()[0]);

		auto encoded = std::make_shared<awst::ARC4Encode>();
		encoded->sourceLocation = m_loc;
		encoded->wtype = elemType;
		encoded->value = std::move(val);

		auto singleArr = std::make_shared<awst::NewArray>();
		singleArr->sourceLocation = m_loc;
		singleArr->wtype = arrWType;
		singleArr->values.push_back(std::move(encoded));

		auto e = std::make_shared<awst::ArrayExtend>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::voidType();
		e->base = writeExpr;
		e->other = std::move(singleArr);
		return e;
	}
	else if (_memberName == "push" && m_call.arguments().empty())
	{
		// push() with no args — box_resize(key, box_len + elemSize)
		unsigned elemSize = builder::StorageMapper::computeEncodedElementSize(elemType);

		auto boxKeyBytes = std::make_shared<awst::BytesConstant>();
		boxKeyBytes->sourceLocation = m_loc;
		boxKeyBytes->wtype = awst::WType::bytesType();
		boxKeyBytes->encoding = awst::BytesEncoding::Utf8;
		boxKeyBytes->value = std::vector<uint8_t>(arrayVarName.begin(), arrayVarName.end());

		// box_len(key) → (uint64, bool)
		auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{awst::WType::uint64Type(), awst::WType::boolType()});
		auto boxLen = std::make_shared<awst::IntrinsicCall>();
		boxLen->sourceLocation = m_loc;
		boxLen->wtype = tupleType;
		boxLen->opCode = "box_len";
		boxLen->stackArgs.push_back(boxKeyBytes);

		auto curSize = std::make_shared<awst::TupleItemExpression>();
		curSize->sourceLocation = m_loc;
		curSize->wtype = awst::WType::uint64Type();
		curSize->base = std::move(boxLen);
		curSize->index = 0;

		auto elemSizeConst = std::make_shared<awst::IntegerConstant>();
		elemSizeConst->sourceLocation = m_loc;
		elemSizeConst->wtype = awst::WType::uint64Type();
		elemSizeConst->value = std::to_string(elemSize);

		auto newSize = std::make_shared<awst::UInt64BinaryOperation>();
		newSize->sourceLocation = m_loc;
		newSize->wtype = awst::WType::uint64Type();
		newSize->left = std::move(curSize);
		newSize->op = awst::UInt64BinaryOperator::Add;
		newSize->right = std::move(elemSizeConst);

		auto boxKeyBytes2 = std::make_shared<awst::BytesConstant>();
		boxKeyBytes2->sourceLocation = m_loc;
		boxKeyBytes2->wtype = awst::WType::bytesType();
		boxKeyBytes2->encoding = awst::BytesEncoding::Utf8;
		boxKeyBytes2->value = std::vector<uint8_t>(arrayVarName.begin(), arrayVarName.end());

		auto resize = std::make_shared<awst::IntrinsicCall>();
		resize->sourceLocation = m_loc;
		resize->wtype = awst::WType::voidType();
		resize->opCode = "box_resize";
		resize->stackArgs.push_back(std::move(boxKeyBytes2));
		resize->stackArgs.push_back(std::move(newSize));

		auto resizeStmt = std::make_shared<awst::ExpressionStatement>();
		resizeStmt->sourceLocation = m_loc;
		resizeStmt->expr = std::move(resize);
		m_ctx.pendingStatements.push_back(std::move(resizeStmt));

		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = m_loc;
		vc->wtype = awst::WType::voidType();
		return vc;
	}
	else if (_memberName == "pop")
	{
		auto popExpr = std::make_shared<awst::ArrayPop>();
		popExpr->sourceLocation = m_loc;
		popExpr->wtype = elemType;
		popExpr->base = writeExpr;

		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = m_loc;
		decode->wtype = rawElemType;
		decode->value = std::move(popExpr);
		return decode;
	}

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = m_loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

std::shared_ptr<awst::Expression> SolArrayMethod::handleMemoryArray(
	std::string const& _memberName,
	Expression const& _baseExpr)
{
	auto base = buildExpr(_baseExpr);

	if (_memberName == "push" && !m_call.arguments().empty())
	{
		auto val = buildExpr(*m_call.arguments()[0]);
		auto* baseWtype = base->wtype;

		// For bytes/string types, push is concat(base, byte)
		if (baseWtype == awst::WType::bytesType()
			|| baseWtype == awst::WType::stringType()
			|| (baseWtype && baseWtype->kind() == awst::WTypeKind::Bytes))
		{
			auto byteVal = val;
			if (byteVal->wtype == awst::WType::uint64Type())
			{
				auto itob = std::make_shared<awst::IntrinsicCall>();
				itob->sourceLocation = m_loc;
				itob->wtype = awst::WType::bytesType();
				itob->opCode = "itob";
				itob->stackArgs.push_back(std::move(byteVal));

				auto seven = std::make_shared<awst::IntegerConstant>();
				seven->sourceLocation = m_loc;
				seven->wtype = awst::WType::uint64Type();
				seven->value = "7";
				auto one = std::make_shared<awst::IntegerConstant>();
				one->sourceLocation = m_loc;
				one->wtype = awst::WType::uint64Type();
				one->value = "1";

				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = m_loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract3";
				extract->stackArgs.push_back(std::move(itob));
				extract->stackArgs.push_back(std::move(seven));
				extract->stackArgs.push_back(std::move(one));
				byteVal = std::move(extract);
			}
			else if (byteVal->wtype != awst::WType::bytesType())
			{
				byteVal = builder::TypeCoercion::stringToBytes(std::move(byteVal), m_loc);
				if (byteVal->wtype != awst::WType::bytesType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = m_loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(byteVal);
					byteVal = std::move(cast);
				}
			}

			auto cat = std::make_shared<awst::IntrinsicCall>();
			cat->sourceLocation = m_loc;
			cat->wtype = baseWtype;
			cat->opCode = "concat";
			cat->stackArgs.push_back(std::move(base));
			cat->stackArgs.push_back(std::move(byteVal));
			return cat;
		}
		else
		{
			// array.push(val) — ArrayExtend
			auto singleArr = std::make_shared<awst::NewArray>();
			singleArr->sourceLocation = m_loc;
			singleArr->wtype = baseWtype;
			singleArr->values.push_back(std::move(val));

			auto e = std::make_shared<awst::ArrayExtend>();
			e->sourceLocation = m_loc;
			e->wtype = awst::WType::voidType();
			e->base = std::move(base);
			e->other = std::move(singleArr);
			return e;
		}
	}
	else if (_memberName == "pop")
	{
		auto e = std::make_shared<awst::ArrayPop>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::voidType();
		e->base = std::move(base);
		return e;
	}

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = m_loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

} // namespace puyasol::builder::sol_ast
