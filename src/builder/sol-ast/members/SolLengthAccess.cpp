/// @file SolLengthAccess.cpp
/// array.length, bytes.length, box-backed array length.
/// Migrated from MemberAccessBuilder.cpp lines 476-555.

#include "builder/sol-ast/members/SolLengthAccess.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <sstream>

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

				// Statically-sized state arrays: `.length` is a compile-time
				// constant, not a box read. Avoid emitting (box_len - 2) /
				// elemSize, which underflows for empty boxes.
				if (!arrType->isDynamicallySized() && !arrType->isByteArrayOrString())
				{
					std::ostringstream oss;
					oss << arrType->length();
					auto c = std::make_shared<awst::IntegerConstant>();
					c->sourceLocation = m_loc;
					// uint256 array sizes (e.g. from erc7201()) don't fit in
					// uint64 — emit as biguint in that case. The result's
					// Solidity type is uint256 which maps to biguint anyway.
					auto solLenType = m_memberAccess.annotation().type;
					if (solLenType && solLenType->category()
							== solidity::frontend::Type::Category::Integer)
					{
						auto const* intType = dynamic_cast<
							solidity::frontend::IntegerType const*>(solLenType);
						if (intType && intType->numBits() > 64)
							c->wtype = awst::WType::biguintType();
						else
							c->wtype = awst::WType::uint64Type();
					}
					else
					{
						c->wtype = arrType->length() > solidity::u256("18446744073709551615")
							? awst::WType::biguintType()
							: awst::WType::uint64Type();
					}
					c->value = oss.str();
					return c;
				}
				auto* rawElemType = m_ctx.typeMapper.map(arrType->baseType());
				auto* arc4ElemType = m_ctx.typeMapper.mapToARC4Type(rawElemType);
				unsigned elemSize = builder::StorageMapper::computeEncodedElementSize(arc4ElemType);

				auto boxKey = awst::makeUtf8BytesConstant(ident->name(), m_loc);

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

				// Dynamic bytes / string state var: the raw box byte count is
				// the Solidity length. No 2-byte ARC4 prefix is applied on
				// write (see `bytes data; data = msg.data;` write path which
				// drops raw bytes into the box), so don't subtract one here.
				if (arrType->isByteArrayOrString())
					return lenVal;

				// Elements with unknown fixed size (e.g. nested dynamic arrays)
				// can't use the `(box_len - 2) / elemSize` trick. Puya's backend
				// doesn't yet model this storage shape — return 0 as a
				// conservative fallback so the AWST at least compiles and
				// the common empty-array case works.
				if (elemSize == 0)
				{
					auto zeroLen = awst::makeIntegerConstant("0", m_loc);
					return zeroLen;
				}

				auto elemSizeConst = awst::makeIntegerConstant(std::to_string(elemSize), m_loc);

				// Guard against box_len returning 0 (uninitialised box):
				// `(0 - 2) / elemSize` underflows. Use `max(len, 2)` so the
				// subtraction always stays non-negative, yielding 0 for
				// empty boxes.
				auto two = awst::makeIntegerConstant("2", m_loc);

				auto lenGe2 = awst::makeNumericCompare(lenVal, awst::NumericComparison::Gte, two, m_loc);

				auto safeLen = std::make_shared<awst::ConditionalExpression>();
				safeLen->sourceLocation = m_loc;
				safeLen->wtype = awst::WType::uint64Type();
				safeLen->condition = std::move(lenGe2);
				safeLen->trueExpr = std::move(lenVal);
				safeLen->falseExpr = std::move(two);

				// Subtract 2-byte ARC4 length header before dividing
				auto headerSize = awst::makeIntegerConstant("2", m_loc);
				auto dataLen = awst::makeUInt64BinOp(std::move(safeLen), awst::UInt64BinaryOperator::Sub, std::move(headerSize), m_loc);

				auto divExpr = awst::makeUInt64BinOp(std::move(dataLen), awst::UInt64BinaryOperator::FloorDiv, std::move(elemSizeConst), m_loc);
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
			auto c = awst::makeIntegerConstant(std::to_string(*fixedBytes->length()), m_loc);
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
