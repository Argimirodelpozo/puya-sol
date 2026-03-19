/// @file IndexAccessBuilder.cpp
/// Handles array/mapping index access and range access expressions.

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

bool ExpressionBuilder::visit(solidity::frontend::IndexAccess const& _node)
{
	auto loc = makeLoc(_node.location());

	// If this is a mapping or dynamic array access, it becomes a box value read.
	// For nested mappings (e.g. _operatorApprovals[owner][operator]), walk up
	// the chain of IndexAccess nodes to find the root Identifier and collect all keys.
	auto const* baseType = _node.baseExpression().annotation().type;
	bool isDynamicArrayAccess = false;
	if (auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(baseType))
	{
		// Check if this is a state variable (box-stored dynamic array)
		if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&_node.baseExpression()))
		{
			if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
			{
				if (varDecl->isStateVariable() && arrType->isDynamicallySized()
				&& !varDecl->isConstant() && !varDecl->immutable())
					isDynamicArrayAccess = true;
			}
		}
	}

	// Also detect nested mapping access: if base is an IndexAccess whose base type is a mapping
	bool isNestedMappingAccess = false;
	if (auto const* baseIndexAccess = dynamic_cast<solidity::frontend::IndexAccess const*>(&_node.baseExpression()))
	{
		auto const* innerBaseType = baseIndexAccess->baseExpression().annotation().type;
		if (innerBaseType && innerBaseType->category() == solidity::frontend::Type::Category::Mapping)
		{
			auto const* innerMapping = dynamic_cast<solidity::frontend::MappingType const*>(innerBaseType);
			if (innerMapping && innerMapping->valueType()->category() == solidity::frontend::Type::Category::Mapping)
				isNestedMappingAccess = true;
		}
	}

	// Box-backed dynamic state array: foo[i] → indexed read from box array
	if (isDynamicArrayAccess)
	{
		auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(baseType);
		auto* rawElemType = m_typeMapper.map(arrType->baseType());
		auto* elemType = m_typeMapper.mapToARC4Type(rawElemType);
		auto* arrWType = m_typeMapper.createType<awst::ReferenceArray>(
			elemType, false, std::nullopt
		);

		std::string arrayVarName;
		if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&_node.baseExpression()))
			arrayVarName = ident->name();

		// Build BoxValueExpression with StateGet default for safe access
		auto boxKey = std::make_shared<awst::BytesConstant>();
		boxKey->sourceLocation = loc;
		boxKey->wtype = awst::WType::boxKeyType();
		boxKey->encoding = awst::BytesEncoding::Utf8;
		boxKey->value = std::vector<uint8_t>(arrayVarName.begin(), arrayVarName.end());

		auto boxExpr = std::make_shared<awst::BoxValueExpression>();
		boxExpr->sourceLocation = loc;
		boxExpr->wtype = arrWType;
		boxExpr->key = boxKey;
		boxExpr->existsAssertionMessage = std::nullopt;

		// For reads, wrap in StateGet with empty array default
		std::shared_ptr<awst::Expression> baseExprForRead = boxExpr;
		if (!_node.annotation().willBeWrittenTo)
		{
			auto emptyArr = std::make_shared<awst::NewArray>();
			emptyArr->sourceLocation = loc;
			emptyArr->wtype = arrWType;

			auto sg = std::make_shared<awst::StateGet>();
			sg->sourceLocation = loc;
			sg->wtype = arrWType;
			sg->field = boxExpr;
			sg->defaultValue = emptyArr;
			baseExprForRead = sg;
		}

		// Build index expression
		auto idx = build(*_node.indexExpression());
		idx = implicitNumericCast(std::move(idx), awst::WType::uint64Type(), loc);

		// IndexExpression on the box array
		auto indexExpr = std::make_shared<awst::IndexExpression>();
		indexExpr->sourceLocation = loc;
		indexExpr->wtype = elemType;
		indexExpr->base = _node.annotation().willBeWrittenTo ? boxExpr : baseExprForRead;
		indexExpr->index = std::move(idx);

		if (_node.annotation().willBeWrittenTo)
		{
			push(indexExpr);
		}
		else
		{
			auto decode = std::make_shared<awst::ARC4Decode>();
			decode->sourceLocation = loc;
			decode->wtype = rawElemType;
			decode->value = std::move(indexExpr);
			push(decode);
		}
		return false;
	}

	if (baseType && (baseType->category() == solidity::frontend::Type::Category::Mapping
		|| isNestedMappingAccess))
	{
		// Collect all index expressions and find root variable name by walking up IndexAccess chain.
		std::vector<solidity::frontend::Expression const*> indexExprs;
		solidity::frontend::Expression const* cursor = &_node;
		std::string varName = "map";

		while (auto const* idxAccess = dynamic_cast<solidity::frontend::IndexAccess const*>(cursor))
		{
			if (idxAccess->indexExpression())
				indexExprs.push_back(idxAccess->indexExpression());
			cursor = &idxAccess->baseExpression();
		}
		// cursor should now be the root Identifier, or a MemberAccess for struct.mapping access
		solidity::frontend::Type const* rootMappingType = nullptr;
		if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(cursor))
		{
			varName = ident->name();
			rootMappingType = ident->annotation().type;
		}
		else if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(cursor))
		{
			// Handle struct.mapping[key] pattern, e.g. self.sideNodes[level]
			// Build varName from the member name (the mapping field name)
			varName = ma->memberName();
			rootMappingType = ma->annotation().type;
		}

		// Reverse so keys are in order: outermost first
		std::reverse(indexExprs.begin(), indexExprs.end());

		// Collect the declared mapping key types by walking the mapping type chain.
		// This ensures key encoding matches the getter (which uses the declared types).
		std::vector<awst::WType const*> declaredKeyWTypes;
		{
			solidity::frontend::Type const* walkType = rootMappingType;
			for (size_t i = 0; i < indexExprs.size(); ++i)
			{
				if (auto const* mt = dynamic_cast<solidity::frontend::MappingType const*>(walkType))
				{
					declaredKeyWTypes.push_back(m_typeMapper.map(mt->keyType()));
					walkType = mt->valueType();
				}
				else
				{
					// Array index or unknown — use nullptr to signal "use translated type"
					declaredKeyWTypes.push_back(nullptr);
					if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(walkType))
						walkType = at->baseType();
					else
						break;
				}
			}
		}

		// Determine the final value type (the non-mapping type at this access level)
		awst::WType const* valueWType = nullptr;
		if (auto const* mappingType = dynamic_cast<solidity::frontend::MappingType const*>(baseType))
		{
			// For an access into a mapping, unwrap one level
			solidity::frontend::Type const* vt = mappingType->valueType();
			// If the value is still a mapping, unwrap further to get the final type
			while (auto const* nested = dynamic_cast<solidity::frontend::MappingType const*>(vt))
				vt = nested->valueType();
			valueWType = m_typeMapper.map(vt);
		}
		else
			valueWType = m_typeMapper.map(_node.annotation().type);

		auto e = std::make_shared<awst::BoxValueExpression>();
		e->sourceLocation = loc;
		e->wtype = valueWType;

		// Build prefix (variable name as box_key)
		auto prefix = std::make_shared<awst::BytesConstant>();
		prefix->sourceLocation = loc;
		prefix->wtype = awst::WType::boxKeyType();
		prefix->encoding = awst::BytesEncoding::Utf8;
		prefix->value = std::vector<uint8_t>(varName.begin(), varName.end());

		if (!indexExprs.empty())
		{
			// Translate all index expressions to bytes
			std::vector<std::shared_ptr<awst::Expression>> keyParts;
			for (size_t ki = 0; ki < indexExprs.size(); ++ki)
			{
				auto const* idxExpr = indexExprs[ki];
				auto translated = build(*idxExpr);

				// Use the declared mapping key type to determine encoding,
				// so that the write path matches the getter's encoding.
				// E.g. mapping(uint256 => ...) always uses 32-byte biguint encoding,
				// even if the literal key fits in uint64.
				awst::WType const* keyWType = (ki < declaredKeyWTypes.size() && declaredKeyWTypes[ki])
					? declaredKeyWTypes[ki]
					: translated->wtype;

				// Coerce translated value to declared key type if they differ
				if (keyWType != translated->wtype)
					translated = implicitNumericCast(std::move(translated), keyWType, loc);

				std::shared_ptr<awst::Expression> keyBytes;
				if (keyWType == awst::WType::uint64Type())
				{
					// uint64 → bytes via itob intrinsic
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(translated));
					keyBytes = std::move(itob);
				}
				else if (keyWType == awst::WType::biguintType())
				{
					// biguint → normalize to exactly 32 bytes before hashing.
					// AVM biguint ops (b+, b-) produce minimal-length bytes via
					// big.Int.Bytes(), while itob produces 8 bytes. Without
					// normalization, the same number gets different sha256 hashes.
					// Pattern: concat(bzero(32), bytes) → extract last 32 bytes.
					auto reinterpret = std::make_shared<awst::ReinterpretCast>();
					reinterpret->sourceLocation = loc;
					reinterpret->wtype = awst::WType::bytesType();
					reinterpret->expr = std::move(translated);

					auto padWidth = std::make_shared<awst::IntegerConstant>();
					padWidth->sourceLocation = loc;
					padWidth->wtype = awst::WType::uint64Type();
					padWidth->value = "32";

					auto pad = std::make_shared<awst::IntrinsicCall>();
					pad->sourceLocation = loc;
					pad->wtype = awst::WType::bytesType();
					pad->opCode = "bzero";
					pad->stackArgs.push_back(std::move(padWidth));

					auto cat = std::make_shared<awst::IntrinsicCall>();
					cat->sourceLocation = loc;
					cat->wtype = awst::WType::bytesType();
					cat->opCode = "concat";
					cat->stackArgs.push_back(std::move(pad));
					cat->stackArgs.push_back(std::move(reinterpret));

					auto lenCall = std::make_shared<awst::IntrinsicCall>();
					lenCall->sourceLocation = loc;
					lenCall->wtype = awst::WType::uint64Type();
					lenCall->opCode = "len";
					lenCall->stackArgs.push_back(cat);

					auto width2 = std::make_shared<awst::IntegerConstant>();
					width2->sourceLocation = loc;
					width2->wtype = awst::WType::uint64Type();
					width2->value = "32";

					auto offset = std::make_shared<awst::IntrinsicCall>();
					offset->sourceLocation = loc;
					offset->wtype = awst::WType::uint64Type();
					offset->opCode = "-";
					offset->stackArgs.push_back(std::move(lenCall));
					offset->stackArgs.push_back(std::move(width2));

					auto width3 = std::make_shared<awst::IntegerConstant>();
					width3->sourceLocation = loc;
					width3->wtype = awst::WType::uint64Type();
					width3->value = "32";

					auto extract = std::make_shared<awst::IntrinsicCall>();
					extract->sourceLocation = loc;
					extract->wtype = awst::WType::bytesType();
					extract->opCode = "extract3";
					extract->stackArgs.push_back(std::move(cat));
					extract->stackArgs.push_back(std::move(offset));
					extract->stackArgs.push_back(std::move(width3));

					keyBytes = std::move(extract);
				}
				else
				{
					// bytes / address / other → ReinterpretCast to bytes
					auto reinterpret = std::make_shared<awst::ReinterpretCast>();
					reinterpret->sourceLocation = loc;
					reinterpret->wtype = awst::WType::bytesType();
					reinterpret->expr = std::move(translated);
					keyBytes = std::move(reinterpret);
				}
				keyParts.push_back(std::move(keyBytes));
			}

			// For a single key, use it directly.
			// For multiple keys (nested mapping), concatenate them.
			std::shared_ptr<awst::Expression> compositeKey;
			if (keyParts.size() == 1)
			{
				compositeKey = std::move(keyParts[0]);
			}
			else
			{
				// Concatenate all key parts: concat(key1, key2, ...)
				compositeKey = std::move(keyParts[0]);
				for (size_t i = 1; i < keyParts.size(); ++i)
				{
					auto concat = std::make_shared<awst::IntrinsicCall>();
					concat->sourceLocation = loc;
					concat->wtype = awst::WType::bytesType();
					concat->opCode = "concat";
					concat->stackArgs.push_back(std::move(compositeKey));
					concat->stackArgs.push_back(std::move(keyParts[i]));
					compositeKey = std::move(concat);
				}
			}

			// Hash the key with sha256 to fit within 64-byte box name limit.
			// prefix(varName) + raw key could exceed 64 bytes
			// (e.g. "registeredUsers" (15) + uint256 (32) = 47 bytes).
			// sha256 produces 32 bytes, so prefix + 32 always fits.
			{
				auto hashCall = std::make_shared<awst::IntrinsicCall>();
				hashCall->sourceLocation = loc;
				hashCall->wtype = awst::WType::bytesType();
				hashCall->opCode = "sha256";
				hashCall->stackArgs.push_back(std::move(compositeKey));
				compositeKey = std::move(hashCall);
			}

			// Build BoxPrefixedKeyExpression
			auto boxKey = std::make_shared<awst::BoxPrefixedKeyExpression>();
			boxKey->sourceLocation = loc;
			boxKey->wtype = awst::WType::boxKeyType();
			boxKey->prefix = prefix;
			boxKey->key = std::move(compositeKey);

			e->key = std::move(boxKey);
		}
		else
		{
			e->key = std::move(prefix);
		}

		// When used as a write target (assignment LHS), push BoxValueExpression directly.
		// When reading, wrap in StateGet with a default so missing boxes return the
		// Solidity default (0/false/empty) instead of asserting existence.
		if (_node.annotation().willBeWrittenTo)
		{
			push(e);
		}
		else
		{
			auto defaultVal = StorageMapper::makeDefaultValue(e->wtype, loc);

			auto stateGet = std::make_shared<awst::StateGet>();
			stateGet->sourceLocation = loc;
			stateGet->wtype = e->wtype;
			stateGet->field = e;
			stateGet->defaultValue = defaultVal;

			push(stateGet);
		}
		return false;
	}

	// Not a mapping/dynamic-array — translate normally
	auto base = build(_node.baseExpression());

	std::shared_ptr<awst::Expression> index;
	if (_node.indexExpression())
		index = build(*_node.indexExpression());

	// Regular array index — ensure index is uint64
	if (index && index->wtype == awst::WType::biguintType())
		index = implicitNumericCast(std::move(index), awst::WType::uint64Type(), loc);

	auto* expectedType = m_typeMapper.map(_node.annotation().type);

	// Determine the actual element type from the base array
	auto* actualElemType = expectedType;
	if (base->wtype && base->wtype->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = static_cast<awst::ReferenceArray const*>(base->wtype);
		actualElemType = const_cast<awst::WType*>(refArr->elementType());
	}

	auto e = std::make_shared<awst::IndexExpression>();
	e->sourceLocation = loc;
	e->base = std::move(base);
	e->index = std::move(index);
	e->wtype = actualElemType;

	// If the element is ARC4 (e.g. from 2D array) but expected type is decoded,
	// wrap in ARC4Decode to convert to the expected reference type
	if (actualElemType != expectedType
		&& actualElemType->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = loc;
		decode->wtype = expectedType;
		decode->value = std::move(e);
		push(decode);
	}
	else
	{
		push(e);
	}
	return false;
}

bool ExpressionBuilder::visit(solidity::frontend::IndexRangeAccess const& _node)
{
	auto loc = makeLoc(_node.location());
	auto base = build(_node.baseExpression());

	std::shared_ptr<awst::Expression> start;
	std::shared_ptr<awst::Expression> end;

	if (_node.startExpression())
		start = build(*_node.startExpression());
	else
	{
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";
		start = std::move(zero);
	}

	if (_node.endExpression())
		end = build(*_node.endExpression());
	else
	{
		// arr[start:] → end = len(arr)
		auto lenCall = std::make_shared<awst::IntrinsicCall>();
		lenCall->sourceLocation = loc;
		lenCall->wtype = awst::WType::uint64Type();
		lenCall->opCode = "len";
		lenCall->stackArgs.push_back(base);
		end = std::move(lenCall);
	}

	// Coerce start/end to uint64 if needed (e.g. biguint from uint256 params)
	start = implicitNumericCast(std::move(start), awst::WType::uint64Type(), loc);
	end = implicitNumericCast(std::move(end), awst::WType::uint64Type(), loc);

	// substring3(base, start, end) — extracts bytes from position start to end
	auto slice = std::make_shared<awst::IntrinsicCall>();
	slice->sourceLocation = loc;
	slice->wtype = m_typeMapper.map(_node.annotation().type);
	slice->opCode = "substring3";
	slice->stackArgs.push_back(std::move(base));
	slice->stackArgs.push_back(std::move(start));
	slice->stackArgs.push_back(std::move(end));
	push(slice);
	return false;
}


} // namespace puyasol::builder
