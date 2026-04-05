/// @file SolArrayBuilder.cpp
/// Solidity typed array builder — handles index access and .length.

#include "builder/sol-eb/SolArrayBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

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
	if (needsDecode)
	{
		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = _loc;
		decode->wtype = expectedType;
		decode->value = std::move(e);
		return std::make_unique<SolArrayBuilder>(m_ctx, m_arrayType, std::move(decode));
	}

	return std::make_unique<SolArrayBuilder>(m_ctx, m_arrayType, std::move(e));
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
