#include "awst/WType.h"

#include <set>

namespace puyasol::awst
{

namespace
{

bool equivalentImpl(
	WType const* lhs,
	WType const* rhs,
	std::set<std::pair<WType const*, WType const*>>& seen)
{
	if (lhs == rhs)
		return true;
	if (!lhs || !rhs || lhs->kind() != rhs->kind()
		|| lhs->immutable() != rhs->immutable())
		return false;
	if (!seen.emplace(lhs, rhs).second)
		return true;

	auto sequenceEqual = [&](auto const& left, auto const& right) {
		if (left.size() != right.size())
			return false;
		for (size_t i = 0; i < left.size(); ++i)
			if (!equivalentImpl(left[i], right[i], seen))
				return false;
		return true;
	};

	switch (lhs->kind())
	{
	case WTypeKind::Bytes:
		return static_cast<BytesWType const*>(lhs)->length()
			== static_cast<BytesWType const*>(rhs)->length();
	case WTypeKind::ARC4UIntN:
	{
		auto const* l = static_cast<ARC4UIntN const*>(lhs);
		auto const* r = static_cast<ARC4UIntN const*>(rhs);
		return l->n() == r->n() && l->arc4Alias() == r->arc4Alias();
	}
	case WTypeKind::ARC4UFixedNxM:
	{
		auto const* l = static_cast<ARC4UFixedNxM const*>(lhs);
		auto const* r = static_cast<ARC4UFixedNxM const*>(rhs);
		return l->n() == r->n() && l->m() == r->m();
	}
	case WTypeKind::ARC4Tuple:
		return sequenceEqual(
			static_cast<ARC4Tuple const*>(lhs)->types(),
			static_cast<ARC4Tuple const*>(rhs)->types());
	case WTypeKind::ARC4DynamicArray:
	{
		auto const* l = static_cast<ARC4DynamicArray const*>(lhs);
		auto const* r = static_cast<ARC4DynamicArray const*>(rhs);
		return l->arc4Alias() == r->arc4Alias()
			&& equivalentImpl(l->elementType(), r->elementType(), seen);
	}
	case WTypeKind::ARC4StaticArray:
	{
		auto const* l = static_cast<ARC4StaticArray const*>(lhs);
		auto const* r = static_cast<ARC4StaticArray const*>(rhs);
		return l->arraySize() == r->arraySize()
			&& l->arc4Alias() == r->arc4Alias()
			&& equivalentImpl(l->elementType(), r->elementType(), seen);
	}
	case WTypeKind::ARC4Struct:
	{
		auto const* l = static_cast<ARC4Struct const*>(lhs);
		auto const* r = static_cast<ARC4Struct const*>(rhs);
		if (l->name() != r->name() || l->frozen() != r->frozen()
			|| l->fields().size() != r->fields().size())
			return false;
		for (size_t i = 0; i < l->fields().size(); ++i)
			if (l->fields()[i].first != r->fields()[i].first
				|| !equivalentImpl(
					l->fields()[i].second, r->fields()[i].second, seen))
				return false;
		return true;
	}
	case WTypeKind::ReferenceArray:
	{
		auto const* l = static_cast<ReferenceArray const*>(lhs);
		auto const* r = static_cast<ReferenceArray const*>(rhs);
		return l->arraySize() == r->arraySize()
			&& equivalentImpl(l->elementType(), r->elementType(), seen);
	}
	case WTypeKind::WTuple:
	{
		auto const* l = static_cast<WTuple const*>(lhs);
		auto const* r = static_cast<WTuple const*>(rhs);
		return l->name() == r->name() && l->names() == r->names()
			&& sequenceEqual(l->types(), r->types());
	}
	default:
		// Basic and transaction types encode their complete identity in name.
		return lhs->name() == rhs->name();
	}
}

} // namespace

bool structurallyEquivalent(WType const* _lhs, WType const* _rhs)
{
	std::set<std::pair<WType const*, WType const*>> seen;
	return equivalentImpl(_lhs, _rhs, seen);
}

namespace
{
// Concrete type for basic singletons (accesses protected WType ctor).
struct BasicWType: public WType
{
	BasicWType(std::string _name, WTypeKind _kind, bool _immutable = true)
		: WType(std::move(_name), _kind, _immutable)
	{
	}
};

// ARC4 basic type — serializes with _type: "ARC4Type" for puya compat.
struct ARC4BasicWType: public WType
{
	ARC4BasicWType(std::string _name)
		: WType(std::move(_name), WTypeKind::Basic, true)
	{
	}
	char const* jsonType() const override { return "ARC4Type"; }
};

// Singleton basic types — allocated once, never freed (static lifetime).
BasicWType const g_voidType("void", WTypeKind::Basic, true);
BasicWType const g_boolType("bool", WTypeKind::Basic, true);
BasicWType const g_uint64Type("uint64", WTypeKind::Basic, true);
BasicWType const g_biguintType("biguint", WTypeKind::Basic, true);
BasicWType const g_stringType("string", WTypeKind::Basic, true);
BytesWType const g_bytesType(std::nullopt);
BasicWType const g_accountType("account", WTypeKind::Basic, true);
BasicWType const g_assetType("asset", WTypeKind::Basic, true);
BasicWType const g_applicationType("application", WTypeKind::Basic, true);
BasicWType const g_stateKeyType("state_key", WTypeKind::Basic, true);
BasicWType const g_boxKeyType("box_key", WTypeKind::Basic, true);
ARC4BasicWType const g_arc4BoolType("arc4.bool");
} // namespace

WType const* WType::voidType() { return &g_voidType; }
WType const* WType::boolType() { return &g_boolType; }
WType const* WType::uint64Type() { return &g_uint64Type; }
WType const* WType::biguintType() { return &g_biguintType; }
WType const* WType::stringType() { return &g_stringType; }
WType const* WType::bytesType() { return &g_bytesType; }
WType const* WType::accountType() { return &g_accountType; }
WType const* WType::assetType() { return &g_assetType; }
WType const* WType::applicationType() { return &g_applicationType; }
WType const* WType::stateKeyType() { return &g_stateKeyType; }
WType const* WType::boxKeyType() { return &g_boxKeyType; }
WType const* WType::arc4BoolType() { return &g_arc4BoolType; }

} // namespace puyasol::awst
