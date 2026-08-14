/// @file EvmSlotLowering.cpp
/// Resolve Solidity storage paths to EVM word addresses. Value reads/writes
/// live in EvmSlotValueLowering.cpp so address derivation and representation
/// policy can evolve independently.

#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/sol-ast/Context.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/storage/SlotWordCodec.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <libsolutil/Keccak256.h>
#include <libsolutil/CommonData.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{

VariableDeclaration const* referencedVar(Expression const& _e)
{
	auto const* id = dynamic_cast<Identifier const*>(&_e);
	if (!id)
		return nullptr;
	return dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration);
}

bool isPersistentStateVar(VariableDeclaration const* _vd)
{
	return _vd && _vd->isStateVariable() && !_vd->isConstant() && !_vd->immutable()
		&& _vd->referenceLocation() != VariableDeclaration::Location::Transient;
}

/// biguint → uint64 (values known < 2^64: page offsets, lengths).
std::shared_ptr<awst::Expression> biguintToU64(
	std::shared_ptr<awst::Expression> _v, awst::SourceLocation const& _loc)
{
	auto cat = awst::makeLeftPad(awst::makeAsBytes(std::move(_v), _loc), 8, _loc);
	return awst::makeBtoi(awst::makeExtractLastN(std::move(cat), 8, _loc), _loc);
}

std::shared_ptr<awst::Expression> toBiguintIndex(
	std::shared_ptr<awst::Expression> _idx, awst::SourceLocation const& _loc)
{
	if (_idx->wtype == awst::WType::uint64Type())
		return awst::makeAsBiguint(awst::makeItob(std::move(_idx), _loc), _loc);
	if (_idx->wtype && _idx->wtype->kind() == awst::WTypeKind::ARC4UIntN)
		return awst::makeARC4Decode(std::move(_idx), awst::WType::biguintType(), _loc);
	return _idx;
}

/// slot (biguint) → its 32-byte big-endian word form.
std::shared_ptr<awst::Expression> slot32Bytes(
	std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc)
{
	return awst::makeLeftPadToN(awst::makeAsBytes(std::move(_slot), _loc), 32, _loc);
}

} // namespace

namespace
{
/// A storage-located local/param — in this mode the variable IS a biguint
/// slot handle (assigned at declaration / by the call convention).
bool isSlotHandleLocal(VariableDeclaration const* _vd)
{
	return _vd && !_vd->isStateVariable()
		&& (_vd->isLocalVariable() || _vd->isCallableOrCatchParameter())
		&& _vd->referenceLocation() == VariableDeclaration::Location::Storage;
}

/// A non-identifier root (library call returning `T storage`, etc.) whose
/// TYPE designates storage — its built value is the biguint slot.
bool isStorageTypedRoot(Expression const& _e)
{
	auto const* t = _e.annotation().type;
	if (!t)
		return false;
	if (dynamic_cast<MappingType const*>(t))
		return true;
	auto const* rt = dynamic_cast<ReferenceType const*>(t);
	return rt && rt->dataStoredIn(DataLocation::Storage);
}
} // namespace

bool EvmSlotLowering::isStorageStateRef(Expression const& _e)
{
	Expression const* cur = &_e;
	for (;;)
	{
		if (auto const* ia = dynamic_cast<IndexAccess const*>(cur))
		{
			cur = &ia->baseExpression();
			continue;
		}
		if (auto const* ma = dynamic_cast<MemberAccess const*>(cur))
		{
			// Contract-qualified state var (`C.x = g`): same slot as bare `x` —
			// resolveMemberAccess handles it, so the shape test must admit it.
			if (auto const* qvd = dynamic_cast<VariableDeclaration const*>(
					ma->annotation().referencedDeclaration);
				qvd && isPersistentStateVar(qvd))
				return true;
			// Only peel STRUCT member access — `.length`/`.push` etc. are not
			// storage lvalue layers.
			if (dynamic_cast<StructType const*>(ma->expression().annotation().type))
			{
				cur = &ma->expression();
				continue;
			}
			return false;
		}
		break;
	}
	if (auto const* vd = referencedVar(*cur))
		return isPersistentStateVar(vd) || isSlotHandleLocal(vd);
	// Non-identifier root: a storage-typed expression (e.g. a library call
	// returning `T storage`) builds to a biguint slot handle in this mode.
	return isStorageTypedRoot(*cur);
}

std::shared_ptr<awst::Expression> EvmSlotLowering::biguintConst(std::string const& _v)
{
	return awst::makeIntegerConstant(_v, m_loc, awst::WType::biguintType());
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readSlotWord(
	std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc)
{
	return SlotHandleAccess::readSlot(std::move(_slot), _loc);
}

std::shared_ptr<awst::Expression> EvmSlotLowering::dynDataBase(
	std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc)
{
	// CONSTANT slot (every declared dynamic array): fold keccak256(slot32) in
	// the compiler — the on-chain hash (130 budget units) never runs. Mirrors
	// the default model's compile-time ArrayData SlotRoutes.
	if (auto const* ic = dynamic_cast<awst::IntegerConstant const*>(_slot.get()))
	{
		solidity::u256 slotVal{ic->value};
		auto k = solidity::u256(solidity::util::keccak256(
			solidity::toBigEndian(slotVal)));
		return awst::makeIntegerConstant(k.str(), _loc, awst::WType::biguintType());
	}
	return awst::makeAsBiguint(
		awst::makeKeccak256(slot32Bytes(std::move(_slot), _loc), _loc), _loc);
}

std::shared_ptr<awst::Expression> EvmSlotLowering::mappingEntrySlot(
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _key,
	Type const* _keyType)
{
	// EVM: entry slot = keccak256(encodedKey ++ slot32). Value-type keys encode
	// as their 32-byte word; string/bytes keys contribute their raw bytes.
	std::shared_ptr<awst::Expression> keyBytes;
	auto const* keyArr = dynamic_cast<ArrayType const*>(_keyType);
	bool isDynamicBytesKey = keyArr && keyArr->isByteArrayOrString();
	if (isDynamicBytesKey)
	{
		keyBytes = _key->wtype == awst::WType::stringType()
			? awst::makeAsBytes(std::move(_key), m_loc)
			: std::move(_key);
		if (keyBytes->wtype != awst::WType::bytesType())
			keyBytes = awst::makeAsBytes(std::move(keyBytes), m_loc);
	}
	else
	{
		// Coerce to the mapped key carrier first (integer literals may build
		// narrower), then take the value's slot-word form.
		auto const* keyW = m_ctx.typeMapper.map(_keyType);
		if (keyW && _key->wtype != keyW
			&& (keyW == awst::WType::uint64Type() || keyW == awst::WType::biguintType()))
			_key = TypeCoercion::implicitNumericCast(std::move(_key), keyW, m_loc);
		auto const* effW = _key->wtype;
		if (auto const* bw = dynamic_cast<awst::BytesWType const*>(effW);
			bw && bw->length().has_value())
		{
			// bytesN keys are LEFT-aligned in the key word (EVM right-pads).
			unsigned n = static_cast<unsigned>(*bw->length());
			keyBytes = awst::makeAsBytes(std::move(_key), m_loc);
			if (n < 32)
				keyBytes = awst::makeConcat(
					std::move(keyBytes), awst::makeBzero(32 - static_cast<int>(n), m_loc), m_loc);
		}
		else
			keyBytes = SlotWordCodec::nativeToPackedBytes(std::move(_key), effW, 32, m_loc);
	}

	auto preimage = awst::makeConcat(
		std::move(keyBytes), slot32Bytes(std::move(_base), m_loc), m_loc);
	return awst::makeAsBiguint(awst::makeKeccak256(std::move(preimage), m_loc), m_loc);
}

EvmSlotLowering::Addr EvmSlotLowering::makeLeafAddr(
	std::shared_ptr<awst::Expression> _slot,
	std::shared_ptr<awst::Expression> _byteOffset,
	unsigned _size,
	bool _aloneInSlot,
	Type const* _solType)
{
	Addr a;
	a.slot = std::move(_slot);
	a.byteOffset = std::move(_byteOffset);
	a.size = _size;
	a.solType = _solType;
	a.wtype = m_ctx.typeMapper.map(_solType);
	// Full-slot AVM account: keep all 32 bytes so real addresses round-trip.
	// (EVM's 20-byte packing survives only where the address shares its slot.)
	if (a.wtype == awst::WType::accountType() && _aloneInSlot && !a.byteOffset)
		a.size = 32;
	return a;
}

std::optional<EvmSlotLowering::Addr> EvmSlotLowering::resolve(Expression const& _e)
{
	if (auto const* id = dynamic_cast<Identifier const*>(&_e))
		return resolveIdentifier(*id);
	if (auto const* ia = dynamic_cast<IndexAccess const*>(&_e))
		return resolveIndexAccess(*ia);
	if (auto const* ma = dynamic_cast<MemberAccess const*>(&_e))
		return resolveMemberAccess(*ma);
	// Ternary of storage refs (`c ? a1 : a2`): the slot is a runtime select.
	// Guarded to BARE state-var branches — those resolve without queueing
	// side effects, so evaluating "both" (as slot constants) is pure; complex
	// branches (m[k], arr[i]) would run the untaken side's key/index effects.
	if (auto const* cond = dynamic_cast<Conditional const*>(&_e))
	{
		auto const* ti = dynamic_cast<Identifier const*>(&cond->trueExpression());
		auto const* fi = dynamic_cast<Identifier const*>(&cond->falseExpression());
		if (ti && fi)
		{
			auto ta = resolve(*ti);
			auto fa = resolve(*fi);
			if (ta && fa)
			{
				auto c = m_ctx.buildExpr(cond->condition());
				if (!c)
					return std::nullopt;
				auto const* t = _e.annotation().type;
				auto sel = awst::makeConditional(std::move(c),
					ta->slot, fa->slot, awst::WType::biguintType(), m_loc);
				return makeLeafAddr(std::move(sel), nullptr,
					t ? t->storageBytes() : 32, /*alone*/ true, t);
			}
		}
	}
	// Type conversion over a storage ref (`bytes(a)` on a storage string):
	// same location, different label — peel it.
	if (auto const* fc = dynamic_cast<FunctionCall const*>(&_e))
		if (fc->annotation().kind.set()
			&& *fc->annotation().kind == FunctionCallKind::TypeConversion
			&& !fc->arguments().empty() && isStorageTypedRoot(_e))
			return resolve(*fc->arguments()[0]);
	// Any other storage-typed root (library call returning `T storage`, a
	// parenthesized/tuple-wrapped ref, ...): its built value IS the slot.
	if (isStorageTypedRoot(_e))
	{
		auto built = m_ctx.buildExpr(_e);
		if (built && built->wtype == awst::WType::biguintType())
		{
			auto const* t = _e.annotation().type;
			return makeLeafAddr(std::move(built), nullptr,
				t ? t->storageBytes() : 32, /*alone*/ true, t);
		}
		Logger::instance().error(
			"--evm-storage-layout: storage-typed expression did not lower to a "
			"slot handle", m_loc);
		return std::nullopt;
	}
	Logger::instance().error(
		"unsupported storage expression shape in --evm-storage-layout", m_loc);
	return std::nullopt;
}

std::optional<EvmSlotLowering::Addr> EvmSlotLowering::addrForStateVar(
	VariableDeclaration const& _vd)
{
	auto const* layout = m_ctx.storageLayout;
	if (!layout)
	{
		Logger::instance().error(
			"--evm-storage-layout: no storage layout in this context (state "
			"variable '" + _vd.name() + "' accessed outside a contract?)", m_loc);
		return std::nullopt;
	}
	auto const* sv = layout->getVarInfoById(_vd.id());
	if (!sv)
	{
		// A declaration owned by ANOTHER contract is not a layout bug: some
		// passes speculatively lower a foreign function body (taking
		// `Other.f.selector` registers f as a function-pointer target) and
		// DISCARD the result — default mode emits nothing for it either. Only
		// a variable that should be OURS (this contract or one of its bases)
		// means the layout walk really missed something.
		auto const* owner = dynamic_cast<solidity::frontend::ContractDefinition const*>(
			_vd.scope());
		auto const* self = layout->contract();
		bool foreign = owner && self && owner != self;
		if (foreign)
			for (auto const* base: self->annotation().linearizedBaseContracts)
				if (base == owner)
					{ foreign = false; break; }
		if (foreign)
		{
			Logger::instance().debug(
				"--evm-storage-layout: skipping foreign state variable '"
				+ _vd.name() + "' (declared in contract " + owner->name()
				+ ", compiling " + self->name() + ")", m_loc);
			return std::nullopt;
		}
		Logger::instance().error(
			"--evm-storage-layout: state variable '" + _vd.name() + "'"
			+ (owner ? " (declared in contract " + owner->name() + ")" : "")
			+ " missing from the storage layout", m_loc);
		return std::nullopt;
	}
	// Alone in its slot (isFullSlot only covers 32-byte vars; a lone address
	// is 20 bytes yet still owns the whole slot).
	bool alone = sv->isFullSlot;
	if (!alone)
		if (auto const* si = layout->getSlotInfo(sv->slot))
			alone = si->variableIndices.size() == 1;
	return makeLeafAddr(
		biguintConst(sv->slot.str()),
		sv->byteOffset
			? awst::makeIntegerConstant(static_cast<uint64_t>(sv->byteOffset), m_loc)
			: nullptr,
		sv->byteSize,
		alone,
		_vd.type());
}

std::optional<EvmSlotLowering::Addr> EvmSlotLowering::resolveIdentifier(
	Identifier const& _id)
{
	auto const* vd = referencedVar(_id);
	if (isSlotHandleLocal(vd))
	{
		// Storage-located local/param: the variable holds the biguint slot
		// (bound at declaration / by the call convention / asm `.slot :=`).
		auto slot = awst::makeVarExpression(
			vd->name(), awst::WType::biguintType(), m_loc);
		return makeLeafAddr(std::move(slot), nullptr,
			vd->type() ? vd->type()->storageBytes() : 32, /*alone*/ true,
			vd->type());
	}
	if (!isPersistentStateVar(vd))
	{
		Logger::instance().error(
			"--evm-storage-layout: cannot resolve '" + _id.name()
			+ "' to a storage slot (not a persistent state variable)", m_loc);
		return std::nullopt;
	}
	return addrForStateVar(*vd);
}

std::optional<EvmSlotLowering::Addr> EvmSlotLowering::resolveIndexAccess(
	IndexAccess const& _ia)
{
	auto const* baseType = _ia.baseExpression().annotation().type;
	if (!_ia.indexExpression())
		return std::nullopt;

	if (auto const* mt = dynamic_cast<MappingType const*>(baseType))
	{
		auto base = resolve(_ia.baseExpression());
		if (!base)
			return std::nullopt;
		auto key = m_ctx.buildExpr(*_ia.indexExpression());
		if (!key)
			return std::nullopt;
		auto slot = mappingEntrySlot(base->slot, std::move(key), mt->keyType());
		auto const* valueType = mt->valueType();
		return makeLeafAddr(std::move(slot), nullptr,
			valueType->storageBytes(), /*alone*/ true, valueType);
	}

	if (auto const* at = dynamic_cast<ArrayType const*>(baseType);
		at && at->dataStoredIn(DataLocation::Storage))
	{
		if (at->isByteArrayOrString())
		{
			Logger::instance().error(
				"--evm-storage-layout: bytes/string element access not yet supported",
				m_loc);
			return std::nullopt;
		}
		auto base = resolve(_ia.baseExpression());
		if (!base)
			return std::nullopt;

		auto idx = toBiguintIndex(m_ctx.buildExpr(*_ia.indexExpression()), m_loc);
		if (!idx)
			return std::nullopt;

		std::shared_ptr<awst::Expression> dataBase;
		if (at->isDynamicallySized())
		{
			// Bounds: idx < length word (EVM Panic 0x32 shape). Pin the index —
			// the assert is a separate pre-statement.
			if (!dynamic_cast<awst::VarExpression const*>(idx.get())
				&& !dynamic_cast<awst::IntegerConstant const*>(idx.get()))
			{
				std::string nm = "__evm_idx_"
					+ std::to_string(awst::NameGen::next("EvmSlotLowering.idx"));
				auto const* idxWt = idx->wtype;   // read BEFORE the move (arg eval order)
				m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(nm, idxWt, m_loc), std::move(idx), m_loc));
				idx = awst::makeVarExpression(nm, awst::WType::biguintType(), m_loc);
			}
			auto len = readSlotWord(base->slot, m_loc);
			auto cmp = awst::makeNumericCompare(
				idx, awst::NumericComparison::Lt, std::move(len), m_loc);
			m_ctx.prePendingStatements.push_back(awst::makeExpressionStatement(
				awst::makeAssert(std::move(cmp), m_loc, "array index out of bounds"), m_loc));
			dataBase = dynDataBase(base->slot, m_loc);
		}
		else
		{
			idx = SlotHandleAccess::boundsCheckIndex(
				m_ctx.prePendingStatements, std::move(idx), at, m_loc);
			dataBase = base->slot;
		}

		return elemAddr(std::move(dataBase), std::move(idx), at->baseType());
	}

	Logger::instance().error(
		"--evm-storage-layout: unsupported index-access base in storage", m_loc);
	return std::nullopt;
}

std::optional<EvmSlotLowering::Addr> EvmSlotLowering::resolveMemberAccess(
	MemberAccess const& _ma)
{
	// Contract-qualified state variable (`C.x`, `Base.y`, `super.z`): the base is
	// a contract/type expression, not a storage struct. It denotes exactly the
	// same slot as the bare identifier, so resolve the referenced declaration.
	if (auto const* qvd = dynamic_cast<VariableDeclaration const*>(
			_ma.annotation().referencedDeclaration);
		qvd && isPersistentStateVar(qvd))
		return addrForStateVar(*qvd);

	auto const* st = dynamic_cast<StructType const*>(_ma.expression().annotation().type);
	if (!st || !st->dataStoredIn(DataLocation::Storage))
	{
		Logger::instance().error(
			"--evm-storage-layout: unsupported member-access base in storage", m_loc);
		return std::nullopt;
	}
	auto base = resolve(_ma.expression());
	if (!base)
		return std::nullopt;

	auto const& off = st->storageOffsetsOfMember(_ma.memberName());
	auto const* fieldType = _ma.annotation().type;
	unsigned size = fieldType ? fieldType->storageBytes() : 32;

	// Alone in its slot? (Decides the full-32 account widening.)
	bool alone = true;
	for (auto const& memberDecl: st->structDefinition().members())
	{
		if (!memberDecl || memberDecl->name() == _ma.memberName())
			continue;
		auto const& mo = st->storageOffsetsOfMember(memberDecl->name());
		if (mo.first == off.first)
		{
			alone = false;
			break;
		}
	}

	auto slot = off.first == 0
		? base->slot
		: awst::makeBigUIntBinOp(base->slot, awst::BigUIntBinaryOperator::Add,
			biguintConst(off.first.str()), m_loc);
	return makeLeafAddr(std::move(slot),
		off.second
			? awst::makeIntegerConstant(static_cast<uint64_t>(off.second), m_loc)
			: nullptr,
		size, alone, fieldType);
}

EvmSlotLowering::Addr EvmSlotLowering::elemAddr(
	std::shared_ptr<awst::Expression> _dataBase,
	std::shared_ptr<awst::Expression> _idx,
	Type const* _elemType)
{
	auto l = SlotHandleAccess::layoutFor(_elemType);
	if (l.strideSlots > 1)
	{
		auto slot = awst::makeBigUIntBinOp(std::move(_dataBase),
			awst::BigUIntBinaryOperator::Add,
			awst::makeBigUIntBinOp(std::move(_idx), awst::BigUIntBinaryOperator::Mult,
				biguintConst(std::to_string(l.strideSlots)), m_loc),
			m_loc);
		return makeLeafAddr(std::move(slot), nullptr, 32, true, _elemType);
	}
	if (l.perSlot <= 1)
	{
		auto slot = awst::makeBigUIntBinOp(std::move(_dataBase),
			awst::BigUIntBinaryOperator::Add, std::move(_idx), m_loc);
		return makeLeafAddr(std::move(slot), nullptr, l.size, true, _elemType);
	}
	// Packed elements: slot = base + idx/perSlot, offset = (idx%perSlot)*size.
	auto idxOnce = awst::makeEvalOnce(std::move(_idx), m_loc);
	auto slot = awst::makeBigUIntBinOp(std::move(_dataBase),
		awst::BigUIntBinaryOperator::Add,
		awst::makeBigUIntBinOp(idxOnce, awst::BigUIntBinaryOperator::FloorDiv,
			biguintConst(std::to_string(l.perSlot)), m_loc),
		m_loc);
	auto within = awst::makeBigUIntBinOp(idxOnce, awst::BigUIntBinaryOperator::Mod,
		biguintConst(std::to_string(l.perSlot)), m_loc);
	auto off = awst::makeUInt64BinOp(biguintToU64(std::move(within), m_loc),
		awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant(static_cast<uint64_t>(l.size), m_loc), m_loc);
	return makeLeafAddr(std::move(slot), std::move(off), l.size, false, _elemType);
}

std::shared_ptr<awst::Expression> EvmSlotLowering::coerceToNative(
	std::shared_ptr<awst::Expression> _value, Addr const& _a)
{
	if (!_value || !_a.wtype || _value->wtype == _a.wtype)
		return _value;
	if (_value->wtype == awst::WType::arc4BoolType()
		&& _a.wtype == awst::WType::boolType())
		return awst::makeARC4Decode(std::move(_value), awst::WType::boolType(), m_loc);
	// ARC4 `address` (byte[32] alias) → native account: the backing bytes ARE
	// the address (struct address fields, Morpho's MarketParams).
	if (_a.wtype == awst::WType::accountType())
		if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_value->wtype);
			sa && sa->arraySize() == 32)
			return awst::makeAsAccount(awst::makeAsBytes(std::move(_value), m_loc), m_loc);
	if (_a.wtype == awst::WType::boolType()
		&& (_value->wtype == awst::WType::uint64Type()
			|| _value->wtype == awst::WType::biguintType()))
		// bool carried numerically (0/1): the word codec's ternary needs a
		// REAL bool condition (frxeth/erc6160 backend rejection).
		return awst::makeNumericCompare(std::move(_value),
			awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", m_loc,
				_value->wtype == awst::WType::biguintType()
					? awst::WType::biguintType() : awst::WType::uint64Type()),
			m_loc);
	if (_value->wtype && _value->wtype->kind() == awst::WTypeKind::ARC4UIntN)
		_value = awst::makeARC4Decode(std::move(_value), awst::WType::biguintType(), m_loc);
	bool valueNum = _value->wtype == awst::WType::uint64Type()
		|| _value->wtype == awst::WType::biguintType();
	bool targetNum = _a.wtype == awst::WType::uint64Type()
		|| _a.wtype == awst::WType::biguintType();
	if (valueNum && targetNum)
		return TypeCoercion::implicitNumericCast(std::move(_value), _a.wtype, m_loc);
	if (auto const* bw = dynamic_cast<awst::BytesWType const*>(_a.wtype);
		bw && bw->length().has_value())
		return TypeCoercion::relabelUnsizedBytes(std::move(_value), _a.wtype, m_loc);
	return _value;
}

} // namespace puyasol::builder::sol_ast
