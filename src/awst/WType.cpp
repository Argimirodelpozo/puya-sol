#include "awst/WType.h"

namespace puyasol::awst
{

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

} // namespace puyasol::awst
