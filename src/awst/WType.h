#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
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

	virtual std::string typeName() const { return m_name; }
	virtual std::string jsonType() const { return "WType"; }
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

	std::string jsonType() const override { return "BytesWType"; }
	std::optional<int> length() const { return m_length; }

private:
	std::optional<int> m_length;
};

class ARC4UIntN: public WType
{
public:
	explicit ARC4UIntN(int _n)
		: WType("arc4.uint" + std::to_string(_n), WTypeKind::ARC4UIntN, true), m_n(_n)
	{
	}

	std::string jsonType() const override { return "ARC4UIntN"; }
	int n() const { return m_n; }

private:
	int m_n;
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

	std::string jsonType() const override { return "ARC4UFixedNxM"; }
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

	std::string jsonType() const override { return "ARC4Tuple"; }
	std::vector<WType const*> const& types() const { return m_types; }

private:
	std::vector<WType const*> m_types;
};

class ARC4DynamicArray: public WType
{
public:
	explicit ARC4DynamicArray(WType const* _elementType)
		: WType(
			  "arc4.dynamic_array<" + _elementType->name() + ">",
			  WTypeKind::ARC4DynamicArray,
			  false
		  ),
		  m_elementType(_elementType)
	{
	}

	std::string jsonType() const override { return "ARC4DynamicArray"; }
	WType const* elementType() const { return m_elementType; }

private:
	WType const* m_elementType;
};

class ARC4StaticArray: public WType
{
public:
	ARC4StaticArray(WType const* _elementType, int _arraySize)
		: WType(
			  "arc4.static_array<" + _elementType->name() + ", "
				  + std::to_string(_arraySize) + ">",
			  WTypeKind::ARC4StaticArray,
			  true
		  ),
		  m_elementType(_elementType),
		  m_arraySize(_arraySize)
	{
	}

	std::string jsonType() const override { return "ARC4StaticArray"; }
	WType const* elementType() const { return m_elementType; }
	int arraySize() const { return m_arraySize; }

private:
	WType const* m_elementType;
	int m_arraySize;
};

class ARC4Struct: public WType
{
public:
	ARC4Struct(
		std::string _name,
		std::map<std::string, WType const*> _fields,
		bool _frozen = false
	)
		: WType(std::move(_name), WTypeKind::ARC4Struct, false),
		  m_fields(std::move(_fields)),
		  m_frozen(_frozen)
	{
	}

	std::string jsonType() const override { return "ARC4Struct"; }
	std::map<std::string, WType const*> const& fields() const { return m_fields; }
	bool frozen() const { return m_frozen; }

private:
	std::map<std::string, WType const*> m_fields;
	bool m_frozen;
};

class ReferenceArray: public WType
{
public:
	explicit ReferenceArray(WType const* _elementType, bool _immutable = false)
		: WType(
			  "array<" + _elementType->name() + ">",
			  WTypeKind::ReferenceArray,
			  _immutable
		  ),
		  m_elementType(_elementType)
	{
	}

	std::string jsonType() const override { return "ReferenceArray"; }
	WType const* elementType() const { return m_elementType; }

private:
	WType const* m_elementType;
};

class WTuple: public WType
{
public:
	WTuple(
		std::vector<WType const*> _types,
		std::optional<std::vector<std::string>> _names = std::nullopt
	)
		: WType("tuple", WTypeKind::WTuple, true),
		  m_types(std::move(_types)),
		  m_names(std::move(_names))
	{
	}

	std::string jsonType() const override { return "WTuple"; }
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
			  false
		  ),
		  m_transactionType(_transactionType)
	{
	}

	std::string jsonType() const override { return "WInnerTransactionFields"; }
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

	std::string jsonType() const override { return "WInnerTransaction"; }
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
