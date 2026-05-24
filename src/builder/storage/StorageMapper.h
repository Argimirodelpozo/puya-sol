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
	// AVM caps a single box's value at 32768 bytes. Fixed-size ARC4 static
	// arrays whose total encoded size exceeds that get split across N boxes
	// keyed `<name>` ++ `itob(page)` (page = 0..N-1). Element accesses
	// route at runtime via `page = idx / elemsPerBox`,
	// `inPageOffset = (idx % elemsPerBox) * elemSize`.

	/// AVM single-box value capacity.
	static constexpr unsigned BOX_VALUE_CAPACITY = 32768;

	/// AVM stack-value maximum (`max_byte_array_size`). Any byte string pushed
	/// to or computed on the stack must fit. `bzero(N)` with N above this cap
	/// reverts at runtime — so puya's `StateGet` default branch can't safely
	/// materialise an all-zero value for box types beyond this size, even
	/// though the box itself can hold up to BOX_VALUE_CAPACITY bytes.
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

	/// `makeStateGet(field, makeDefaultValue(type, loc), type, loc)` for most
	/// types. Returns `Expression` (not `StateGet`) because for box-backed
	/// reads of types that overflow AVM's 4 KB stack-value cap (statically
	/// or at runtime — dyn arrays/bytes/oversized fixed arrays) we skip the
	/// StateGet wrapper and return the bare `BoxValueExpression`. See
	/// `createStateRead` / puyabug.md §4c/4d for the rationale.
	/// Wraps the
	/// common "read storage slot, fall back to type default" pattern. Pass the
	/// stored wtype explicitly because some callers read with a wtype that
	/// differs from the field's own wtype (multi-page arrays, struct slot
	/// promotion, etc).
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
};

} // namespace puyasol::builder
