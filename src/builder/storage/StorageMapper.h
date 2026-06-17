#pragma once

#include "awst/Node.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <string>
#include <vector>

namespace puyasol::builder
{

/// Maps Solidity state variables to AWST AppStorageDefinitions.
class StorageMapper
{
public:
	explicit StorageMapper(TypeMapper& _typeMapper): m_typeMapper(_typeMapper) {}

	/// Create AppStorageDefinitions for a contract's state variables.
	std::vector<awst::AppStorageDefinition> mapStateVariables(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _sourceFile
	);

	/// Create an expression to read a state variable.
	std::shared_ptr<awst::Expression> createStateRead(
		std::string const& _varName,
		awst::WType const* _type,
		awst::AppStorageKind _kind,
		awst::SourceLocation const& _loc
	);

	/// Create an expression to write a state variable.
	std::shared_ptr<awst::Expression> createStateWrite(
		std::string const& _varName,
		std::shared_ptr<awst::Expression> _value,
		awst::WType const* _type,
		awst::AppStorageKind _kind,
		awst::SourceLocation const& _loc
	);

	/// Determine if a variable should use box storage.
	static bool shouldUseBoxStorage(solidity::frontend::VariableDeclaration const& _var);

	/// Compute the fixed encoded byte size of an AWST element type.
	/// Returns 0 for variable-length types (skip splitting).
	static int computeEncodedElementSize(awst::WType const* _type);

	// ── Multi-box storage for oversized fixed arrays ──
	// ARC4StaticArrays exceeding AVM's 32768-byte box cap split across N boxes
	// keyed `<name>++itob(page)`. Element access: page=idx/elemsPerBox,
	// offset=(idx%elemsPerBox)*elemSize.

	/// AVM single-box value capacity.
	static constexpr unsigned BOX_VALUE_CAPACITY = 32768;

	/// AVM stack-value cap (`max_byte_array_size`). bzero(N>4096) reverts at
	/// runtime, so StateGet's zero-default can't be materialised for large box types.
	static constexpr int kAvmStackValueMax = 4096;

	/// Element size for an ARC4StaticArray's fixed element type, or 0 if
	/// the element isn't a fixed-encoded scalar (e.g. dynamic ARC4).
	static unsigned arc4StaticArrayElementSize(awst::WType const* _type);

	/// Total encoded bytes for an ARC4StaticArray (`element_size * count`).
	/// Returns 0 if the type isn't ARC4StaticArray or has variable element size.
	static uint64_t arc4StaticArrayTotalBytes(awst::WType const* _type);

	/// Returns true if the array type's encoded size exceeds a single
	/// box's capacity and therefore requires the multi-box layout.
	static bool isMultiBoxArray(awst::WType const* _type);

	/// Number of boxes needed to back a multi-box array, or 1 for single-box.
	static unsigned numBoxesForArray(awst::WType const* _type);

	/// Number of elements per box for a multi-box array
	/// (`floor(BOX_VALUE_CAPACITY / element_size)`).
	static unsigned elementsPerBox(awst::WType const* _type);

	/// Create a type-correct default value expression (0/false/empty) for the given wtype.
	static std::shared_ptr<awst::Expression> makeDefaultValue(
		awst::WType const* _type,
		awst::SourceLocation const& _loc
	);

	/// StateGet(field, default) for most types. For box-backed types that exceed
	/// AVM's 4 KB stack-value cap (oversized fixed arrays, dyn arrays/bytes),
	/// returns the bare BoxValueExpression instead — see puyabug.md §4c/4d.
	/// Pass wtype explicitly; some callers read with a wtype different from the
	/// field's own (multi-page arrays, struct slot promotion).
	static std::shared_ptr<awst::Expression> makeStateGetWithDefault(
		std::shared_ptr<awst::Expression> _field,
		awst::WType const* _type,
		awst::SourceLocation const& _loc
	);

	/// Convert a biguint slot expression to a uint64 via btoi(extract3(reinterpret(slot), len-8, 8)).
	static std::shared_ptr<awst::Expression> biguintSlotToBtoi(
		std::shared_ptr<awst::Expression> const& _slotExpr,
		awst::SourceLocation const& _loc
	);

	/// Canonical top-level state-var box: BoxValueExpression keyed by _varName.
	/// Pairs with isTopLevelDynamicBox to recognise the shape.
	static std::shared_ptr<awst::BoxValueExpression> makeTopLevelBoxExpr(
		std::string const& _varName,
		awst::WType const* _type,
		awst::SourceLocation const& _loc
	);

	/// True iff _box is a top-level dynamic-typed state-var box
	/// (ARC4DynamicArray / ReferenceArray / dynamic bytes) eagerly created in
	/// __postInit (m_boxArrayVarNames), so bare BoxValueExpression reads are safe.
	/// "Top-level" = key is a BytesConstant; mapping values (runtime concat/hash)
	/// are lazy and don't qualify. Shared by makeStateGetWithDefault (read skip)
	/// and handleDelete (box_put-empty instead of box_del).
	static bool isTopLevelDynamicBox(awst::BoxValueExpression const* _box);

	/// True for mapping-derived keys: BoxPrefixedKey or sha256 IntrinsicCall.
	/// Gates pre-create-per-entry-box logic for `mapping(K => sized_type)`.
	static bool isMappingDerivedKey(awst::Expression const* _key);

	/// box_len(<key>) as WTuple(uint64, bool); callers pick item 0 (len) or 1 (exists).
	static std::shared_ptr<awst::Expression> makeBoxLenTuple(
		TypeMapper& _typeMapper,
		std::shared_ptr<awst::Expression> _key,
		awst::SourceLocation const& _loc);

private:
	TypeMapper& m_typeMapper;

	awst::SourceLocation makeLoc(
		solidity::langutil::SourceLocation const& _solLoc,
		std::string const& _file
	);

	std::shared_ptr<awst::BytesConstant> makeKeyExpr(
		std::string const& _name,
		awst::SourceLocation const& _loc,
		awst::AppStorageKind _kind = awst::AppStorageKind::AppGlobal
	);

	/// Raw storage target: BoxValueExpression (Box) or AppStateExpression (AppGlobal).
	/// Shared by createStateRead (adds exists-assert/StateGet) and createStateWrite.
	std::shared_ptr<awst::Expression> makeStorageTarget(
		std::shared_ptr<awst::BytesConstant> const& _key,
		awst::WType const* _type,
		awst::AppStorageKind _kind,
		awst::SourceLocation const& _loc
	);
};

} // namespace puyasol::builder
