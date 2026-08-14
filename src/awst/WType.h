#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace puyasol::awst
{

/// Mirrors puya's awst/wtypes.py — the AWST type system.
/// Basic types are singletons; parameterized types are constructed on demand.

enum class WTypeKind
{
	Basic,
	Bytes,
	ARC4UIntN,
	ARC4UFixedNxM,
	ARC4Tuple,
	ARC4DynamicArray,
	ARC4StaticArray,
	ARC4Struct,
	ReferenceArray,
	WTuple,
	WGroupTransaction,
	WInnerTransactionFields,
	WInnerTransaction,
};

class WType
{
public:
	virtual ~WType() = default;

	virtual char const* jsonType() const { return "WType"; }
	virtual bool immutable() const { return m_immutable; }
	std::string const& name() const { return m_name; }
	WTypeKind kind() const { return m_kind; }

	// Basic singleton types
	static WType const* voidType();
	static WType const* boolType();
	static WType const* uint64Type();
	static WType const* biguintType();
	static WType const* stringType();
	static WType const* bytesType();
	static WType const* accountType();
	static WType const* assetType();
	static WType const* applicationType();
	static WType const* stateKeyType();
	static WType const* boxKeyType();
	static WType const* arc4BoolType();

	/// Force this type to report as immutable, preventing puya's implicit
	/// mutable arg threading. Used when we handle write-back explicitly.
	void forceImmutable() { m_immutable = true; }

protected:
	WType(std::string _name, WTypeKind _kind, bool _immutable = true)
		: m_name(std::move(_name)), m_kind(_kind), m_immutable(_immutable)
	{
	}

	std::string m_name;
	WTypeKind m_kind;
	bool m_immutable;
};

class BytesWType: public WType
{
public:
	explicit BytesWType(std::optional<int> _length = std::nullopt)
		: WType(
			  _length ? "bytes[" + std::to_string(*_length) + "]" : "bytes",
			  WTypeKind::Bytes,
			  true
		  ),
		  m_length(_length)
	{
	}

	char const* jsonType() const override { return "BytesWType"; }
	std::optional<int> length() const { return m_length; }

private:
	std::optional<int> m_length;
};

/// Guarded bytes-view helpers. INVARIANT: BytesWType is the ONLY class with
/// kind()==Bytes (bytesType() itself is a BytesWType{nullopt} singleton), so a
/// kind check and a dynamic_cast agree — but sites used to mix them, including
/// `kind==Bytes && dynamic_cast<...>(t)->length()` combos that deref the cast
/// unguarded. Route through these instead of hand-rolling the cast.

/// Typed bytes view of _t, or nullptr when _t isn't a bytes wtype.
inline BytesWType const* asBytesWType(WType const* _t)
{
	return dynamic_cast<BytesWType const*>(_t);
}

/// Engaged with N iff _t is a SIZED `bytes[N]`; nullopt for dynamic `bytes`
/// and for non-bytes wtypes alike.
inline std::optional<int> fixedBytesLength(WType const* _t)
{
	if (auto const* bw = asBytesWType(_t))
		return bw->length();
	return std::nullopt;
}

/// True iff _t is the DYNAMIC bytes wtype (kind Bytes, no length) — covers the
/// bytesType() singleton and any other unsized BytesWType instance.
inline bool isDynamicBytes(WType const* _t)
{
	auto const* bw = asBytesWType(_t);
	return bw && !bw->length().has_value();
}

/// True iff _t is one of the two NATIVE integer tiers (uint64-backed N<=64 or
/// biguint-backed N>64). The per-tier singleton compares (`== uint64Type()`,
/// `== biguintType()`) remain the idiomatic single-tier checks — this exists
/// for the "is it an integer at all" question so the pair isn't re-spelled.
inline bool isNumericWType(WType const* _t)
{
	return _t == WType::uint64Type() || _t == WType::biguintType();
}

class ARC4UIntN: public WType
{
public:
	explicit ARC4UIntN(int _n, std::string _arc4Alias = "")
		: WType(
			_arc4Alias.empty()
				? "arc4.uint" + std::to_string(_n)
				: "arc4." + _arc4Alias,
			WTypeKind::ARC4UIntN, true
		  ),
		  m_n(_n),
		  m_arc4Alias(std::move(_arc4Alias))
	{
	}

	char const* jsonType() const override { return "ARC4UIntN"; }
	int n() const { return m_n; }
	std::string const& arc4Alias() const { return m_arc4Alias; }

	/// Whether this ARC4 integer is SIGNED. Signedness is encoded in the alias:
	/// signed types carry `"int" + bits` (e.g. `int128`), unsigned types have an
	/// empty alias (the name is derived as `arc4.uint<n>`). This is the single
	/// typed accessor for that fact — callers must NOT re-derive it by string
	/// slicing `arc4Alias()` (was `substr(0,3) == "int"` at ~6 sites).
	bool isSigned() const { return m_arc4Alias.rfind("int", 0) == 0; }

private:
	int m_n;
	std::string m_arc4Alias;
};

class ARC4UFixedNxM: public WType
{
public:
	ARC4UFixedNxM(int _n, int _m)
		: WType(
			  "arc4.ufixed" + std::to_string(_n) + "x" + std::to_string(_m),
			  WTypeKind::ARC4UFixedNxM,
			  true
		  ),
		  m_n(_n),
		  m_m(_m)
	{
	}

	char const* jsonType() const override { return "ARC4UFixedNxM"; }
	int n() const { return m_n; }
	int m() const { return m_m; }

private:
	int m_n;
	int m_m;
};

class ARC4Tuple: public WType
{
public:
	explicit ARC4Tuple(std::vector<WType const*> _types)
		: WType("arc4.tuple", WTypeKind::ARC4Tuple, true), m_types(std::move(_types))
	{
	}

	char const* jsonType() const override { return "ARC4Tuple"; }
	std::vector<WType const*> const& types() const { return m_types; }

private:
	std::vector<WType const*> m_types;
};

class ARC4DynamicArray: public WType
{
public:
	explicit ARC4DynamicArray(WType const* _elementType, std::string _arc4Alias = {})
		: WType(
			  _arc4Alias.empty()
				  ? "arc4.dynamic_array<" + _elementType->name() + ">"
				  : _arc4Alias,
			  WTypeKind::ARC4DynamicArray,
			  false // ARC4 arrays are mutable (matching puya Python default)
		  ),
		  m_elementType(_elementType),
		  m_arc4Alias(std::move(_arc4Alias))
	{
	}

	char const* jsonType() const override { return "ARC4DynamicArray"; }
	WType const* elementType() const { return m_elementType; }
	std::string const& arc4Alias() const { return m_arc4Alias; }

private:
	WType const* m_elementType;
	std::string m_arc4Alias;
};

class ARC4StaticArray: public WType
{
public:
	ARC4StaticArray(WType const* _elementType, int64_t _arraySize, std::string _arc4Alias = {})
		: WType(
			  _arc4Alias.empty()
				  ? "arc4.static_array<" + _elementType->name() + ", "
					    + std::to_string(_arraySize) + ">"
				  : _arc4Alias,
			  WTypeKind::ARC4StaticArray,
			  false // ARC4 arrays are mutable (matching puya Python default)
		  ),
		  m_elementType(_elementType),
		  m_arraySize(_arraySize),
		  m_arc4Alias(std::move(_arc4Alias))
	{
	}

	char const* jsonType() const override { return "ARC4StaticArray"; }
	WType const* elementType() const { return m_elementType; }
	int64_t arraySize() const { return m_arraySize; }
	std::string const& arc4Alias() const { return m_arc4Alias; }

private:
	WType const* m_elementType;
	int64_t m_arraySize;
	std::string m_arc4Alias;
};

class ARC4Struct: public WType
{
public:
	ARC4Struct(
		std::string _name,
		std::vector<std::pair<std::string, WType const*>> _fields,
		bool _frozen = false
	)
		: WType(std::move(_name), WTypeKind::ARC4Struct,
			_frozen && std::all_of(_fields.begin(), _fields.end(),
				[](auto const& p) { return p.second->immutable(); })),
		  m_fields(std::move(_fields)),
		  m_frozen(_frozen)
	{
	}

	char const* jsonType() const override { return "ARC4Struct"; }
	std::vector<std::pair<std::string, WType const*>> const& fields() const { return m_fields; }
	bool frozen() const { return m_frozen; }

private:
	std::vector<std::pair<std::string, WType const*>> m_fields;
	bool m_frozen;
};

class ReferenceArray: public WType
{
public:
	explicit ReferenceArray(
		WType const* _elementType,
		bool _immutable = true,
		std::optional<int64_t> _arraySize = std::nullopt
	)
		: WType(
			  _arraySize
				  ? "array<" + _elementType->name() + ", " + std::to_string(*_arraySize) + ">"
				  : "array<" + _elementType->name() + ">",
			  WTypeKind::ReferenceArray,
			  _immutable
		  ),
		  m_elementType(_elementType),
		  m_arraySize(_arraySize)
	{
	}

	char const* jsonType() const override { return "ReferenceArray"; }
	WType const* elementType() const { return m_elementType; }
	std::optional<int64_t> arraySize() const { return m_arraySize; }

private:
	WType const* m_elementType;
	std::optional<int64_t> m_arraySize;
};

class WTuple: public WType
{
public:
	WTuple(
		std::vector<WType const*> _types,
		std::optional<std::vector<std::string>> _names = std::nullopt,
		std::string _name = "tuple"
	)
		: WType(std::move(_name), WTypeKind::WTuple, true),
		  m_types(std::move(_types)),
		  m_names(std::move(_names))
	{
	}

	char const* jsonType() const override { return "WTuple"; }
	std::vector<WType const*> const& types() const { return m_types; }
	std::optional<std::vector<std::string>> const& names() const { return m_names; }

private:
	std::vector<WType const*> m_types;
	std::optional<std::vector<std::string>> m_names;
};

class WInnerTransactionFields: public WType
{
public:
	explicit WInnerTransactionFields(std::optional<int> _transactionType = std::nullopt)
		: WType(
			  _transactionType
				  ? "inner_transaction_fields_" + txnTypeSuffix(*_transactionType)
				  : std::string("inner_transaction_fields"),
			  WTypeKind::WInnerTransactionFields,
			  true
		  ),
		  m_transactionType(_transactionType)
	{
	}

	char const* jsonType() const override { return "WInnerTransactionFields"; }
	std::optional<int> transactionType() const { return m_transactionType; }

private:
	static std::string txnTypeSuffix(int _type)
	{
		switch (_type)
		{
		case 1: return "pay";
		case 2: return "keyreg";
		case 3: return "acfg";
		case 4: return "axfer";
		case 5: return "afrz";
		case 6: return "appl";
		default: return "unknown";
		}
	}
	std::optional<int> m_transactionType;
};

class WInnerTransaction: public WType
{
public:
	explicit WInnerTransaction(std::optional<int> _transactionType = std::nullopt)
		: WType(
			  _transactionType
				  ? "inner_transaction_" + txnTypeSuffix(*_transactionType)
				  : std::string("inner_transaction"),
			  WTypeKind::WInnerTransaction,
			  true
		  ),
		  m_transactionType(_transactionType)
	{
	}

	char const* jsonType() const override { return "WInnerTransaction"; }
	std::optional<int> transactionType() const { return m_transactionType; }

private:
	static std::string txnTypeSuffix(int _type)
	{
		switch (_type)
		{
		case 1: return "pay";
		case 2: return "keyreg";
		case 3: return "acfg";
		case 4: return "axfer";
		case 5: return "afrz";
		case 6: return "appl";
		default: return "unknown";
		}
	}
	std::optional<int> m_transactionType;
};

} // namespace puyasol::awst
