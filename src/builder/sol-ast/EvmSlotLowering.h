#pragma once

/// @file EvmSlotLowering.h
/// `--evm-storage-layout` lowering: every storage access rooted at a state
/// variable resolves to a WORD ADDRESS in the flat EVM slot space — a biguint
/// slot plus an intra-slot (byteOffset, size) window — and reads/writes go
/// through __puyasol___storage_read/__storage_write (hybrid paged/sparse
/// boxes, see EvmLayoutMode.h). Slot derivation follows Solidity exactly:
/// declared vars at StorageLayout positions, mapping entries at
/// keccak256(key32 ++ slot32), dynamic-array data at keccak256(slot32).
/// Because assembly sload/sstore hits the same subroutines, slot arithmetic
/// in Yul (OZ StorageSlot / Checkpoints idioms) addresses the same state.

#include "awst/Node.h"
#include "builder/sol-eb/ContractContext.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <optional>

namespace puyasol::builder::sol_ast
{

class EvmSlotLowering
{
public:
	EvmSlotLowering(
		eb::ContractContext& _ctx,
		Context& _scope,
		awst::SourceLocation const& _loc)
		: m_ctx(_ctx), m_scope(_scope), m_loc(_loc)
	{}

	/// Cheap AST-shape test (no expression built): does `_e` peel through
	/// index/member layers to a PERSISTENT state variable? Constants,
	/// immutables and transient vars are excluded (they are not in slot space).
	static bool isStorageStateRef(solidity::frontend::Expression const& _e);

	/// True when an access chain peels through any number of array indices and
	/// struct members to an expression whose runtime representation is a
	/// biguint slot handle.  This includes registered storage locals/parameters,
	/// computed storage refs returned by `.slot` assembly helpers, and every
	/// storage ref in EVM-layout mode.  Keeping the representation test here
	/// lets scalar and aggregate reads/writes share the same recursive lowering.
	static bool isSlotHandleRef(
		solidity::frontend::Expression const& _e,
		eb::ContractContext& _ctx,
		Context& _scope);

	/// A leaf word address: `slot` (biguint) with an intra-slot byte window.
	/// `byteOffset` null means 0. `size`==32 with zero offset is the aligned
	/// full-word fast path.
	struct Addr
	{
		std::shared_ptr<awst::Expression> slot;
		std::shared_ptr<awst::Expression> byteOffset;
		unsigned size = 32;
		solidity::frontend::Type const* solType = nullptr;
		awst::WType const* wtype = nullptr;
	};

	/// Resolve a storage expression to its word address. Bounds asserts are
	/// queued to ctx.preEffects(). On unsupported shapes: loud error +
	/// nullopt (NEVER fall back to the named-cell model — split-brain state).
	std::optional<Addr> resolve(solidity::frontend::Expression const& _e);

	/// Word address of a state variable itself (ctor initializers, getters).
	std::optional<Addr> addrForStateVar(
		solidity::frontend::VariableDeclaration const& _vd);

	/// Word address of element `_idx` (biguint) in an array whose DATA region
	/// starts at `_dataBase` (no bounds check — callers own that).
	Addr elemAddr(
		std::shared_ptr<awst::Expression> _dataBase,
		std::shared_ptr<awst::Expression> _idx,
		solidity::frontend::Type const* _elemType);

	/// Coerce a built value to the leaf's native carrier (numeric casts, ARC4
	/// decode, unsized-bytes relabel) — what writeValue expects.
	std::shared_ptr<awst::Expression> coerceToNative(
		std::shared_ptr<awst::Expression> _value, Addr const& _a);

	/// Is this a storage `bytes`/`string` leaf (EVM short/long encoded)?
	static bool isBytesLike(solidity::frontend::Type const* _t);

	/// Whole bytes/string value at `_a.slot` via __evm_bytes_read (short/long
	/// format). Returns `string`-typed when the leaf is a string.
	std::shared_ptr<awst::Expression> readBytesValue(Addr const& _a);

	/// Whole-array write into storage. Dynamic-array chains use the recursive
	/// runtime codec (writes lengths + elements and clears shrink tails); fixed
	/// arrays (len<=64) recurse per element. Returns false with a diagnostic for
	/// representations the codec cannot express.
	/// `delete` on an aggregate: zero the value-slot span, recurse into
	/// bytes/array members for their keccak-region data, dynamic arrays via
	/// __evm_dynarr_write with an empty payload (EVM delete clears elements).
	/// Write a value of ANY declared type to a slot address: scalars via the
	/// word codec, bytes/string via the short/long subroutines, arrays and
	/// structs via their element writers. One dispatch shared by every
	/// storage-write site (scalar assignment, tuple components, state
	/// initializers) so a new shape never lands in only one of them.
	bool writeAny(
		Addr& _a,
		solidity::frontend::Type const* _t,
		std::shared_ptr<awst::Expression> _value,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// Read a value of any materialisable declared type from a slot address.
	/// This is the read-side twin of writeAny and is the recursion point used by
	/// aggregate containers; mappings deliberately have no value materialisation.
	std::shared_ptr<awst::Expression> readAny(
		Addr const& _a, solidity::frontend::Type const* _t);

	bool clearAggregate(
		Addr const& _a,
		solidity::frontend::Type const* _t,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	bool writeArrayValue(
		Addr const& _a,
		solidity::frontend::ArrayType const* _at,
		std::shared_ptr<awst::Expression> _value,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// Whole bytes/string write via __evm_bytes_write (clears stale chunks).
	void writeBytesValue(
		Addr const& _a,
		std::shared_ptr<awst::Expression> _value,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// Materialise a whole STRUCT at `_a.slot` as a NewStruct value (per-slot
	/// word reads via SlotHandleAccess::readStructElem; temps go to
	/// ctx.preEffects()). Null + loud error when `_a` isn't a struct.
	std::shared_ptr<awst::Expression> readStructValue(Addr const& _a);

	/// Materialise an array at `_a.slot`; dynamic-array chains use the runtime
	/// codec and fixed arrays recurse with an unroll cap of 64 per level.
	std::shared_ptr<awst::Expression> readArrayValue(
		Addr const& _a, solidity::frontend::ArrayType const* _at);

	/// Whole-struct WRITE: split `_value` (struct-typed) into per-member slot
	/// writes at `_a.slot`, recursing into NESTED struct members. Statements
	/// appended to `_out`; loud error + false for unsupported member types.
	bool writeStructValue(
		Addr const& _a,
		std::shared_ptr<awst::Expression> _value,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// Leaf read: storage word → native value (Addr::wtype).
	std::shared_ptr<awst::Expression> readValue(Addr const& _a);

	/// Leaf write: statements appended to `_out`. `_value` must already be the
	/// native repr of Addr::wtype (sub-word writes read-modify-write the word).
	void writeValue(
		Addr const& _a,
		std::shared_ptr<awst::Expression> _value,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// __puyasol___storage_read(slot) — the raw biguint word.
	static std::shared_ptr<awst::Expression> readSlotWord(
		std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc);

	/// Dynamic array/bytes data region base: keccak256(slot32) as biguint.
	static std::shared_ptr<awst::Expression> dynDataBase(
		std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc);

	/// The mapping-entry slot: keccak256(encodedKey ++ slot32). `_key` is the
	/// BUILT key expression; encoding is the EVM one (value types → their
	/// 32-byte word; string/bytes → raw bytes).
	std::shared_ptr<awst::Expression> mappingEntrySlot(
		std::shared_ptr<awst::Expression> _base,
		std::shared_ptr<awst::Expression> _key,
		solidity::frontend::Type const* _keyType);

private:
	std::optional<Addr> resolveIdentifier(solidity::frontend::Identifier const& _id);
	std::optional<Addr> resolveIndexAccess(solidity::frontend::IndexAccess const& _ia);
	std::optional<Addr> resolveMemberAccess(solidity::frontend::MemberAccess const& _ma);

	/// Leaf window for a type at `slot`: full-slot `account` widens to 32 bytes
	/// so real 32-byte AVM addresses round-trip (the trailing-20 EVM packing is
	/// only used when the address genuinely shares its slot).
	Addr makeLeafAddr(
		std::shared_ptr<awst::Expression> _slot,
		std::shared_ptr<awst::Expression> _byteOffset,
		unsigned _size,
		bool _aloneInSlot,
		solidity::frontend::Type const* _solType);

	std::shared_ptr<awst::Expression> biguintConst(std::string const& _v);

	eb::ContractContext& m_ctx;
	Context& m_scope;
	awst::SourceLocation m_loc;
};

} // namespace puyasol::builder::sol_ast
