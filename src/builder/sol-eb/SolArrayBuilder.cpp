/// @file SolArrayBuilder.cpp
/// Solidity typed array builder — handles index access and .length.

#include "builder/sol-eb/SolArrayBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::eb
{

awst::WType const* SolArrayBuilder::elementType() const
{
	auto* baseWType = wtype();
	if (!baseWType)
		return nullptr;

	switch (baseWType->kind())
	{
	case awst::WTypeKind::ReferenceArray:
		return static_cast<awst::ReferenceArray const*>(baseWType)->elementType();
	case awst::WTypeKind::ARC4DynamicArray:
		return static_cast<awst::ARC4DynamicArray const*>(baseWType)->elementType();
	case awst::WTypeKind::ARC4StaticArray:
		return static_cast<awst::ARC4StaticArray const*>(baseWType)->elementType();
	default:
		return nullptr;
	}
}

std::unique_ptr<InstanceBuilder> SolArrayBuilder::index(
	InstanceBuilder& _idx, awst::SourceLocation const& _loc)
{
	auto base = resolve();
	auto index = _idx.resolve();

	// Ensure index is uint64
	if (index->wtype == awst::WType::biguintType())
		index = TypeCoercion::implicitNumericCast(std::move(index), awst::WType::uint64Type(), _loc);

	auto* elemType = elementType();
	if (!elemType)
		return nullptr;

	auto e = std::make_shared<awst::IndexExpression>();
	e->sourceLocation = _loc;
	e->base = std::move(base);
	e->index = std::move(index);
	e->wtype = elemType;

	// Determine if we need to decode ARC4 → native
	auto* expectedType = m_ctx.typeMapper.map(m_arrayType->baseType());
	bool needsDecode = elemType != expectedType
		&& (elemType->kind() == awst::WTypeKind::ARC4StaticArray
			|| elemType->kind() == awst::WTypeKind::ARC4UIntN
			|| elemType->kind() == awst::WTypeKind::ARC4DynamicArray
			|| elemType->kind() == awst::WTypeKind::ARC4Struct);

	std::shared_ptr<awst::Expression> result = std::move(e);
	if (needsDecode)
	{
		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = _loc;
		decode->wtype = expectedType;
		decode->value = std::move(result);
		result = std::move(decode);
	}

	// Enum element validation: Solidity panics (0x21) when reading an out-
	// of-range enum from an array. Spill the read to a local so the assert
	// sees the same value and survives DCE on `arr[i];` expression statements.
	if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(
			m_arrayType->baseType()))
	{
		unsigned numMembers = enumType->numberOfMembers();
		static int enumCheckCounter = 0;
		std::string tmpName = "__enum_idx_" + std::to_string(enumCheckCounter++);

		auto tmpVar = std::make_shared<awst::VarExpression>();
		tmpVar->sourceLocation = _loc;
		tmpVar->wtype = result->wtype;
		tmpVar->name = tmpName;

		auto assignTmp = std::make_shared<awst::AssignmentStatement>();
		assignTmp->sourceLocation = _loc;
		assignTmp->target = tmpVar;
		assignTmp->value = result;
		m_ctx.prePendingStatements.push_back(std::move(assignTmp));

		auto cmpLhs = TypeCoercion::implicitNumericCast(
			tmpVar, awst::WType::uint64Type(), _loc);
		auto maxVal = std::make_shared<awst::IntegerConstant>();
		maxVal->sourceLocation = _loc;
		maxVal->wtype = awst::WType::uint64Type();
		maxVal->value = std::to_string(numMembers);

		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(cmpLhs);
		cmp->op = awst::NumericComparison::Lt;
		cmp->rhs = std::move(maxVal);

		auto assertStmt = std::make_shared<awst::ExpressionStatement>();
		assertStmt->sourceLocation = _loc;
		assertStmt->expr = awst::makeAssert(std::move(cmp), _loc, "Enum out of range");
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));

		result = tmpVar;
	}

	return std::make_unique<SolArrayBuilder>(m_ctx, m_arrayType, std::move(result));
}

std::unique_ptr<NodeBuilder> SolArrayBuilder::member_access(
	std::string const& _name, awst::SourceLocation const& _loc)
{
	if (_name == "length")
	{
		auto base = resolve();
		// Use ArrayLength node for ReferenceArray and ARC4 arrays
		auto kind = base->wtype ? base->wtype->kind() : awst::WTypeKind::Bytes;
		if (kind == awst::WTypeKind::ReferenceArray
			|| kind == awst::WTypeKind::ARC4StaticArray
			|| kind == awst::WTypeKind::ARC4DynamicArray)
		{
			auto e = std::make_shared<awst::ArrayLength>();
			e->sourceLocation = _loc;
			e->wtype = awst::WType::uint64Type();
			e->array = std::move(base);
			return std::make_unique<SolArrayBuilder>(m_ctx, m_arrayType, std::move(e));
		}
		// For other types (bytes): use len intrinsic
		auto len = std::make_shared<awst::IntrinsicCall>();
		len->sourceLocation = _loc;
		len->wtype = awst::WType::uint64Type();
		len->opCode = "len";
		len->stackArgs.push_back(std::move(base));
		return std::make_unique<SolArrayBuilder>(m_ctx, m_arrayType, std::move(len));
	}

	// .push, .pop, etc. — not yet handled by builder, fall through
	return nullptr;
}

} // namespace puyasol::builder::eb
