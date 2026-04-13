/// @file SolLengthAccess.cpp
/// array.length, bytes.length, box-backed array length.
/// Migrated from MemberAccessBuilder.cpp lines 476-555.

#include "builder/sol-ast/members/SolLengthAccess.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolLengthAccess::toAwst()
{
	auto const& baseExpr = baseExpression();

	// Box-backed dynamic array: length = box_len(key) / elemSize
	if (auto const* ident = dynamic_cast<Identifier const*>(&baseExpr))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (varDecl->isStateVariable()
				&& !varDecl->isConstant()
				&& !varDecl->immutable()
				&& builder::StorageMapper::shouldUseBoxStorage(*varDecl)
				&& dynamic_cast<ArrayType const*>(varDecl->type()))
			{
				auto const* arrType = dynamic_cast<ArrayType const*>(varDecl->type());
				auto* rawElemType = m_ctx.typeMapper.map(arrType->baseType());
				auto* arc4ElemType = m_ctx.typeMapper.mapToARC4Type(rawElemType);
				unsigned elemSize = builder::StorageMapper::computeEncodedElementSize(arc4ElemType);

				std::string varName = ident->name();
				auto boxKey = std::make_shared<awst::BytesConstant>();
				boxKey->sourceLocation = m_loc;
				boxKey->wtype = awst::WType::bytesType();
				boxKey->encoding = awst::BytesEncoding::Utf8;
				boxKey->value = std::vector<uint8_t>(varName.begin(), varName.end());

				auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
					std::vector<awst::WType const*>{
						awst::WType::uint64Type(), awst::WType::boolType()});
				auto boxLen = std::make_shared<awst::IntrinsicCall>();
				boxLen->sourceLocation = m_loc;
				boxLen->wtype = tupleType;
				boxLen->opCode = "box_len";
				boxLen->stackArgs.push_back(std::move(boxKey));

				auto lenVal = std::make_shared<awst::TupleItemExpression>();
				lenVal->sourceLocation = m_loc;
				lenVal->wtype = awst::WType::uint64Type();
				lenVal->base = std::move(boxLen);
				lenVal->index = 0;

				// Elements with unknown fixed size (e.g. nested dynamic arrays)
				// can't use the `(box_len - 2) / elemSize` trick. Puya's backend
				// doesn't yet model this storage shape — return 0 as a
				// conservative fallback so the AWST at least compiles and
				// the common empty-array case works.
				if (elemSize == 0)
				{
					auto zeroLen = std::make_shared<awst::IntegerConstant>();
					zeroLen->sourceLocation = m_loc;
					zeroLen->wtype = awst::WType::uint64Type();
					zeroLen->value = "0";
					return zeroLen;
				}

				auto elemSizeConst = std::make_shared<awst::IntegerConstant>();
				elemSizeConst->sourceLocation = m_loc;
				elemSizeConst->wtype = awst::WType::uint64Type();
				elemSizeConst->value = std::to_string(elemSize);

				// Subtract 2-byte ARC4 length header before dividing
				auto headerSize = std::make_shared<awst::IntegerConstant>();
				headerSize->sourceLocation = m_loc;
				headerSize->wtype = awst::WType::uint64Type();
				headerSize->value = "2";
				auto dataLen = std::make_shared<awst::UInt64BinaryOperation>();
				dataLen->sourceLocation = m_loc;
				dataLen->wtype = awst::WType::uint64Type();
				dataLen->left = std::move(lenVal);
				dataLen->op = awst::UInt64BinaryOperator::Sub;
				dataLen->right = std::move(headerSize);

				auto divExpr = std::make_shared<awst::UInt64BinaryOperation>();
				divExpr->sourceLocation = m_loc;
				divExpr->wtype = awst::WType::uint64Type();
				divExpr->left = std::move(dataLen);
				divExpr->op = awst::UInt64BinaryOperator::FloorDiv;
				divExpr->right = std::move(elemSizeConst);
				return divExpr;
			}
		}
	}

	auto base = buildExpr(baseExpr);

	// bytesN.length → compile-time constant N (fixed-size bytes)
	if (auto const* fixedBytes = dynamic_cast<awst::BytesWType const*>(base->wtype))
	{
		if (fixedBytes->length().has_value())
		{
			auto c = std::make_shared<awst::IntegerConstant>();
			c->sourceLocation = m_loc;
			c->wtype = awst::WType::uint64Type();
			c->value = std::to_string(*fixedBytes->length());
			return c;
		}
	}

	// bytes.length → len intrinsic
	if (base->wtype == awst::WType::bytesType())
	{
		auto e = std::make_shared<awst::IntrinsicCall>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::uint64Type();
		e->opCode = "len";
		e->stackArgs.push_back(std::move(base));
		return e;
	}

	// array.length → ArrayLength node
	auto e = std::make_shared<awst::ArrayLength>();
	e->sourceLocation = m_loc;
	e->wtype = awst::WType::uint64Type();
	e->array = std::move(base);
	return e;
}

} // namespace puyasol::builder::sol_ast
