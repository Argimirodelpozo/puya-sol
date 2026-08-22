/// @file EvmSlotValueLowering.cpp
/// Reading, materialising, writing, and clearing values after
/// EvmSlotLowering has resolved their storage word addresses.

#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/sol-ast/Context.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/storage/SlotWordCodec.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <libsolutil/Keccak256.h>
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{
/// Shadow slot carrying the HIGH 12 bytes of a PACKED address (a 20-byte
/// window cannot hold a 32-byte AVM address; the word keeps the EVM-shaped
/// trailing-20 so asm sees EVM layout, and Solidity reads recombine).
/// keccak-derived, so it can never collide with declared or mapping slots.
std::shared_ptr<awst::Expression> packedAddrAuxSlot(
	std::shared_ptr<awst::Expression> _slot,
	std::shared_ptr<awst::Expression> const& _byteOffset,
	awst::SourceLocation const& _loc)
{
	auto pre = awst::makeConcat(
		awst::makeLeftPadToN(awst::makeAsBytes(std::move(_slot), _loc), 32, _loc),
		_byteOffset
			? std::shared_ptr<awst::Expression>(awst::makeItob(_byteOffset, _loc))
			: std::shared_ptr<awst::Expression>(
				awst::makeBytesConstant(std::vector<uint8_t>(8, 0), _loc)),
		_loc);
	pre = awst::makeConcat(std::move(pre),
		awst::makeUtf8BytesConstant("addraux", _loc), _loc);
	return awst::makeAsBiguint(awst::makeKeccak256(std::move(pre), _loc), _loc);
}
} // namespace

namespace
{
/// ARC4Decode target for a bytes-like leaf: puya type-checks the decode, so a
/// Solidity `string` (arc4 len+utf8[]) must decode to `string`, not `bytes`.
awst::WType const* bytesLikeDecodeTarget(solidity::frontend::Type const* _t)
{
	if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(_t))
		if (at->isString())
			return awst::WType::stringType();
	return awst::WType::bytesType();
}
} // namespace

bool EvmSlotLowering::isBytesLike(Type const* _t)
{
	auto const* at = dynamic_cast<ArrayType const*>(_t);
	return at && at->isByteArrayOrString();
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readBytesValue(Addr const& _a)
{
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___evm_bytes_read"},
		awst::WType::bytesType(), m_loc);
	awst::pushCallArg(call->args, "__slot", _a.slot);
	auto const* at = dynamic_cast<ArrayType const*>(_a.solType);
	if (at && at->isString())
		return awst::makeReinterpretCast(
			std::move(call), awst::WType::stringType(), m_loc);
	return call;
}

void EvmSlotLowering::writeBytesValue(
	Addr const& _a,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (_value && _value->wtype != awst::WType::bytesType())
		_value = awst::makeAsBytes(std::move(_value), m_loc);
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___evm_bytes_write"},
		awst::WType::voidType(), m_loc);
	awst::pushCallArg(call->args, "__slot", _a.slot);
	awst::pushCallArg(call->args, "__val", std::move(_value));
	_out.push_back(awst::makeExpressionStatement(std::move(call), m_loc));
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readStructValue(Addr const& _a)
{
	auto const* st = dynamic_cast<StructType const*>(_a.solType);
	auto const* structW = st
		? dynamic_cast<awst::ARC4Struct const*>(m_ctx.typeMapper.map(st)) : nullptr;
	if (!st || !structW)
	{
		Logger::instance().error(
			"--evm-storage-layout: cannot materialise non-struct storage "
			"aggregate as a value", m_loc);
		return nullptr;
	}
	// Aggregate members recurse from their member-offset bases. Mappings have no
	// materialised value in Solidity and are therefore deliberately skipped.
	bool anyNested = false;
	for (auto const& m: st->structDefinition().members())
	{
		if (!m || !m->type())
			continue;
		if (dynamic_cast<StructType const*>(m->type()))
			anyNested = true;
		else if (isBytesLike(m->type()))
			anyNested = true;   // string/bytes member → recursive path below
		else if (dynamic_cast<ArrayType const*>(m->type()))
			anyNested = true;   // array member → readArrayValue below
		else if (dynamic_cast<solidity::frontend::MappingType const*>(m->type()))
			anyNested = true;   // mapping member → SKIPPED (Solidity does too)
		else if (!m->type()->isValueType())
		{
			Logger::instance().error(
				"--evm-storage-layout: cannot materialise struct '"
				+ st->structDefinition().name() + "' as a value — member '"
				+ m->name() + "' is not a value type", m_loc);
			return nullptr;
		}
	}
	if (!anyNested)
		return SlotHandleAccess::readStructElem(
			m_ctx.preEffects(), _a.slot, st, structW, m_loc);

	// pin the base once — members read in separate sub-expressions
	std::string nm = "__evm_stv_"
		+ std::to_string(awst::NameGen::next("EvmSlotLowering.structVal"));
	m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(nm, awst::WType::biguintType(), m_loc),
		_a.slot, m_loc));
	auto baseVar = [&]() {
		return awst::makeVarExpression(nm, awst::WType::biguintType(), m_loc);
	};
	auto ns = awst::makeNewStruct(structW, m_loc);
	for (auto const& m: st->structDefinition().members())
	{
		if (!m)
			continue;
		auto const& off = st->storageOffsetsOfMember(m->name());
		Addr fa;
		fa.slot = off.first == 0
			? std::shared_ptr<awst::Expression>(baseVar())
			: std::shared_ptr<awst::Expression>(awst::makeBigUIntBinOp(
				baseVar(), awst::BigUIntBinaryOperator::Add,
				biguintConst(off.first.str()), m_loc));
		fa.solType = m->type();
		if (auto const* nst = dynamic_cast<StructType const*>(m->type()))
		{
			fa.wtype = m_ctx.typeMapper.map(nst);
			auto v = readStructValue(fa);
			if (!v)
				return nullptr;
			ns->values[m->name()] = std::move(v);
			continue;
		}
		if (dynamic_cast<solidity::frontend::MappingType const*>(m->type()))
			continue;   // mapping content is addressed by keccak paths, never copied
		if (auto const* mat = dynamic_cast<ArrayType const*>(m->type());
			mat && !isBytesLike(m->type()))
		{
			fa.wtype = m_ctx.typeMapper.map(m->type());
			auto v = readArrayValue(fa, mat);
			if (!v)
				return nullptr;
			awst::WType const* fieldW3 = nullptr;
			for (auto const& [fname, ftype]: structW->fields())
				if (fname == m->name()) { fieldW3 = ftype; break; }
			if (fieldW3 && v->wtype != fieldW3)
				v = awst::makeARC4Encode(std::move(v), fieldW3, m_loc);
			ns->values[m->name()] = std::move(v);
			continue;
		}
		if (isBytesLike(m->type()))
		{
			// string/bytes member: EVM short/long format at the member slot
			fa.wtype = m_ctx.typeMapper.map(m->type());
			auto v = readBytesValue(fa);
			awst::WType const* fieldW2 = nullptr;
			for (auto const& [fname, ftype]: structW->fields())
				if (fname == m->name()) { fieldW2 = ftype; break; }
			if (v && fieldW2 && v->wtype != fieldW2)
				v = awst::makeARC4Encode(std::move(v), fieldW2, m_loc);
			if (!v)
				return nullptr;
			ns->values[m->name()] = std::move(v);
			continue;
		}
		fa.byteOffset = off.second
			? awst::makeIntegerConstant(static_cast<uint64_t>(off.second), m_loc)
			: nullptr;
		fa.size = m->type()->storageBytes();
		fa.wtype = m_ctx.typeMapper.map(m->type());
		if (fa.wtype == awst::WType::accountType() && !fa.byteOffset)
			fa.size = fa.size;   // keep declared width inside structs
		// ARC4 struct fields carry ARC4 wtypes — convert the native read.
		awst::WType const* fieldW = nullptr;
		for (auto const& [fname, ftype]: structW->fields())
			if (fname == m->name()) { fieldW = ftype; break; }
		auto v = readValue(fa);
		if (v && fieldW && v->wtype != fieldW)
		{
			if (fieldW->kind() == awst::WTypeKind::ARC4UIntN
				|| fieldW == awst::WType::arc4BoolType())
				v = awst::makeARC4Encode(std::move(v), fieldW, m_loc);
		}
		if (!v)
			return nullptr;
		ns->values[m->name()] = std::move(v);
	}
	return ns;
}

bool EvmSlotLowering::writeStructValue(
	Addr const& _a,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	auto const* st = dynamic_cast<StructType const*>(_a.solType);
	auto const* structW = st
		? dynamic_cast<awst::ARC4Struct const*>(m_ctx.typeMapper.map(st)) : nullptr;
	if (!st || !structW || !_value)
	{
		Logger::instance().error(
			"--evm-storage-layout: unsupported whole-struct storage write", m_loc);
		return false;
	}
	// pin base slot + struct value: members write in separate statements
	std::string bs = "__evm_stw_"
		+ std::to_string(awst::NameGen::next("EvmSlotLowering.structW"));
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc),
		_a.slot, m_loc));
	std::string vs = bs + "_v";
	auto const* valW = _value->wtype ? _value->wtype
		: static_cast<awst::WType const*>(structW);
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(vs, valW, m_loc), std::move(_value), m_loc));
	auto baseVar = [&]() {
		return awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc);
	};
	auto valVar = [&]() {
		return awst::makeVarExpression(vs, valW, m_loc);
	};
	for (auto const& m: st->structDefinition().members())
	{
		if (!m)
			continue;
		auto const& off = st->storageOffsetsOfMember(m->name());
		Addr fa;
		fa.slot = off.first == 0
			? std::shared_ptr<awst::Expression>(baseVar())
			: std::shared_ptr<awst::Expression>(awst::makeBigUIntBinOp(
				baseVar(), awst::BigUIntBinaryOperator::Add,
				biguintConst(off.first.str()), m_loc));
		fa.solType = m->type();
		awst::WType const* fieldW = nullptr;
		for (auto const& [fname, ftype]: structW->fields())
			if (fname == m->name()) { fieldW = ftype; break; }
		auto field = awst::makeFieldExpression(valVar(), m->name(),
			fieldW ? fieldW : m_ctx.typeMapper.map(m->type()), m_loc);
		if (auto const* nst = dynamic_cast<StructType const*>(m->type()))
		{
			fa.wtype = m_ctx.typeMapper.map(nst);
			if (!writeStructValue(fa, std::move(field), _out))
				return false;
			continue;
		}
		if (isBytesLike(m->type()))
		{
			fa.wtype = m_ctx.typeMapper.map(m->type());
			std::shared_ptr<awst::Expression> fv = std::move(field);
			if (fv->wtype && fv->wtype->kind() != awst::WTypeKind::Bytes
				&& fv->wtype != awst::WType::stringType())
				fv = awst::makeARC4Decode(std::move(fv),
					bytesLikeDecodeTarget(m->type()), m_loc);
			writeBytesValue(fa, std::move(fv), _out);
			continue;
		}
		if (dynamic_cast<solidity::frontend::MappingType const*>(m->type()))
			continue;   // Solidity skips mapping members on struct assignment
		if (auto const* mat2 = dynamic_cast<ArrayType const*>(m->type());
			mat2 && !isBytesLike(m->type()))
		{
			fa.wtype = m_ctx.typeMapper.map(m->type());
			if (!writeArrayValue(fa, mat2, std::move(field), _out))
				return false;
			continue;
		}
		if (!m->type()->isValueType())
		{
			Logger::instance().error(
				"--evm-storage-layout: whole-struct write with non-value member '"
				+ m->name() + "' is not yet supported", m_loc);
			return false;
		}
		fa.byteOffset = off.second
			? awst::makeIntegerConstant(static_cast<uint64_t>(off.second), m_loc)
			: nullptr;
		fa.size = m->type()->storageBytes();
		fa.wtype = m_ctx.typeMapper.map(m->type());
		auto v = coerceToNative(std::move(field), fa);
		if (!v)
			return false;
		writeValue(fa, std::move(v), _out);
	}
	return true;
}

namespace
{
/// (storage bytes, ARC4 bytes, elements-per-slot) for a dynamic array element.
/// ARC4 width differs from storage width for `address` (20 stored, 32 encoded)
/// and for sub-word ints only when the ARC4 alias rounds up — both are read
/// straight off the mapped element wtype so the two never drift.
struct DynElemMetrics
{
	unsigned size = 32;
	unsigned arc4Width = 32;
	unsigned perSlot = 1;
	unsigned lanesPerElem = 1;   // fixed-array / uniform-struct elements: the
	                             // element is this many LANES, identical in
	                             // slot layout and ARC4 concatenation order
	bool bitPacked = false;       // fixed bool[N]: byte lanes in EVM storage,
	                              // MSB-first bits in each ARC4 element
	bool ok = false;
};

DynElemMetrics dynElemMetrics(
	solidity::frontend::Type const* _elemType, awst::WType const* _elemW)
{
	DynElemMetrics m;
	if (!_elemType || !_elemW)
		return m;
	// FIXED-array element (uint256[2], uint24[3]): lanes are the inner
	// scalars. Accepted when the lanes are full words (any element slot
	// count) or the whole element packs into ONE slot — both keep the global
	// lane index aligned with the per-slot packing math.
	if (auto const* fat = dynamic_cast<solidity::frontend::ArrayType const*>(_elemType);
		fat && !fat->isDynamicallySized() && !fat->isByteArrayOrString())
	{
		auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_elemW);
		if (!sa)
			return m;
		// Solidity gives every bool one storage byte, while ARC4 packs a
		// bool[N] into ceil(N/8) bytes, MSB-first. A fixed array used as a
		// dynamic-array element starts on a fresh EVM slot, so support the
		// one-slot form here and let the runtime codec reset its ARC4 bit
		// cursor at every outer-element boundary.
		if (dynamic_cast<solidity::frontend::BoolType const*>(fat->baseType())
			&& sa->elementType() == awst::WType::arc4BoolType())
		{
			auto lanesU = fat->length();
			if (lanesU == 0 || lanesU > 32)
				return m;
			unsigned lanes = static_cast<unsigned>(lanesU);
			m.size = 1;
			m.arc4Width = (lanes + 7u) / 8u;
			m.perSlot = lanes;
			m.lanesPerElem = lanes;
			m.bitPacked = true;
			m.ok = true;
			return m;
		}
		auto inner = dynElemMetrics(fat->baseType(), sa->elementType());
		if (!inner.ok || inner.bitPacked)
			return m;
		auto lanesU = fat->length();
		if (lanesU == 0 || lanesU > 64
			|| inner.lanesPerElem > 64 / static_cast<unsigned>(lanesU))
			return m;
		unsigned lanes = static_cast<unsigned>(lanesU);
		if (inner.lanesPerElem == 1
			&& inner.size != 32 && lanes * inner.size > 32)
			return m;
		m.size = inner.size;
		m.arc4Width = inner.arc4Width;
		// A nested fixed array starts on the same storage alignment as its
		// immediate child. Preserve that child's lanes-per-slot and multiply
		// only the logical lanes in one outer element. This recursively flattens
		// uint8[2][3][4] (and full-word equivalents) without assuming a rank.
		m.perSlot = inner.lanesPerElem == 1
			? (inner.size == 32 ? 1 : lanes)
			: inner.perSlot;
		m.lanesPerElem = lanes * inner.lanesPerElem;
		m.ok = true;
		return m;
	}
	// Uniform STRUCT element (struct S { uint256 a; uint256 b; }): every
	// member a full-slot 32-byte value whose ARC4 encoding is its canonical
	// word — the element is a plain word concatenation.
	if (auto const* st = dynamic_cast<solidity::frontend::StructType const*>(_elemType))
	{
		auto const* sw = dynamic_cast<awst::ARC4Struct const*>(_elemW);
		if (!sw)
			return m;
		unsigned lanes = 0;
		for (auto const& mem: st->structDefinition().members())
		{
			if (!mem || !mem->type())
				return m;
			auto const* mt = mem->type();
			if (!mt->isValueType() || mt->storageBytes() != 32)
				return m;
			if (dynamic_cast<solidity::frontend::FixedBytesType const*>(mt) == nullptr
				&& mt->category() != solidity::frontend::Type::Category::Integer)
				return m;
			++lanes;
		}
		if (lanes == 0 || lanes > 64)
			return m;
		m.size = 32;
		m.arc4Width = 32;
		m.perSlot = 1;
		m.lanesPerElem = lanes;
		m.ok = true;
		return m;
	}
	if (_elemType->isDynamicallySized()
		|| !_elemType->isValueType())
		return m;
	m.size = puyasol::builder::SlotHandleAccess::layoutFor(_elemType).size;
	if (m.size == 0 || m.size > 32)
		return m;
	bool const isAddress = _elemW == awst::WType::accountType()
		|| _elemW->name() == "address";
	// An address ELEMENT of a dynamic array occupies a whole slot on its own
	// (20 bytes packs 1-per-slot either way), and the writer stores the full
	// 32-byte Algorand account there. Slicing the EVM 20-byte width back out
	// silently dropped the account's high 12 bytes, so the aggregate read
	// disagreed with the element-wise read of the SAME storage. Addresses are
	// Algorand accounts here, so take the whole word.
	//
	// Deliberately scoped to the alone-in-a-slot case — the same rule the
	// packed-slot-address getter uses. An address PACKED beside other fields
	// keeps its 20-byte EVM width, so struct layouts stay asm-compatible.
	if (isAddress && m.size == 20)
		m.size = 32;
	m.perSlot = 32u / m.size;
	if (auto const* ui = dynamic_cast<awst::ARC4UIntN const*>(_elemW))
		m.arc4Width = static_cast<unsigned>(ui->n()) / 8u;
	else if (isAddress)
		m.arc4Width = 32;
	else if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_elemW))
		m.arc4Width = static_cast<unsigned>(sa->arraySize());
	else if (_elemW == awst::WType::arc4BoolType())
		return m;   // ARC4 bool is BIT-packed — not a byte-width element
	else
		m.arc4Width = m.size;
	if (m.arc4Width < m.size)
		return m;
	m.ok = true;
	return m;
}

struct DynamicArrayChain
{
	unsigned depth = 0;
	DynElemMetrics leafMetrics;
};

DynamicArrayChain dynamicArrayChain(
	solidity::frontend::ArrayType const* _root,
	awst::WType const* _rootW)
{
	using namespace solidity::frontend;
	DynamicArrayChain result;
	auto const* array = _root;
	auto const* arrayW = dynamic_cast<awst::ARC4DynamicArray const*>(_rootW);
	while (array && arrayW && array->isDynamicallySized()
		&& !array->isByteArrayOrString())
	{
		++result.depth;
		auto const* element = array->baseType();
		auto const* elementW = arrayW->elementType();
		auto const* nested = dynamic_cast<ArrayType const*>(element);
		if (!nested || !nested->isDynamicallySized()
			|| nested->isByteArrayOrString())
		{
			result.leafMetrics = dynElemMetrics(element, elementW);
			break;
		}
		array = nested;
		arrayW = dynamic_cast<awst::ARC4DynamicArray const*>(elementW);
	}
	return result;
}
} // namespace

std::shared_ptr<awst::Expression> EvmSlotLowering::readArrayValue(
	Addr const& _a, ArrayType const* _at)
{
	if (!_at)
		return nullptr;
	if (_at->isDynamicallySized())
	{
		// Consecutive dynamic-array layers use one depth-driven recursive
		// codec. The leaf metrics describe the first non-dynamic-array element,
		// so T[], T[][] and T[][][] differ only by the depth argument.
		auto const* arrW = m_ctx.typeMapper.map(_at);
		auto chain = dynamicArrayChain(_at, arrW);
		if (chain.depth == 0 || !chain.leafMetrics.ok)
		{
			// Mixed aggregate tree (e.g. T[][2][], string[], or struct-with-
			// array[]): emit the type-directed loop here. The homogeneous
			// dynamic-chain helper below remains the compact fast path, while this
			// fallback recursively delegates each child to readAny.
			auto const* elemType = _at->baseType();
			if (dynamic_cast<MappingType const*>(elemType))
			{
				Logger::instance().error(
					"--evm-storage-layout: mappings cannot be materialised as array values",
					m_loc);
				return nullptr;
			}
			auto const* elemArc4 = m_ctx.typeMapper.mapSolTypeToARC4(elemType);
			bool const elemDynamic = arc4IsDynamic(elemArc4);
			int const elemSize = computeEncodedElementSize(elemArc4);
			if (!elemArc4 || (!elemDynamic && elemSize <= 0))
			{
				Logger::instance().error(
					"--evm-storage-layout: array element has no representable ARC4 encoding",
					m_loc);
				return nullptr;
			}

			int uid = awst::NameGen::next("EvmSlotLowering.genericArrayRead");
			auto name = [&](char const* tag) {
				return std::string("__evm_gar_") + tag + "_" + std::to_string(uid);
			};
			std::string slotN = name("slot"), dataN = name("base"), nN = name("n"),
				iN = name("i"), headsN = name("heads"), tailsN = name("tails"),
				offN = name("off"), innerN = name("inner"), resultN = name("result");
			auto bv = [&](std::string const& n) {
				return awst::makeVarExpression(n, awst::WType::biguintType(), m_loc);
			};
			auto uv = [&](std::string const& n) {
				return awst::makeVarExpression(n, awst::WType::uint64Type(), m_loc);
			};
			auto xv = [&](std::string const& n) {
				return awst::makeVarExpression(n, awst::WType::bytesType(), m_loc);
			};
			auto u64c = [&](uint64_t v) {
				return awst::makeIntegerConstant(v, m_loc);
			};
			auto toU64 = [&](std::shared_ptr<awst::Expression> v) {
				return awst::makeBtoi(awst::makeExtractLastN(
					awst::makeZeroExtendToN(awst::makeAsBytes(std::move(v), m_loc),
						8, m_loc), 8, m_loc), m_loc);
			};
			auto asBigIndex = [&](std::shared_ptr<awst::Expression> v) {
				return awst::makeAsBiguint(awst::makeItob(std::move(v), m_loc), m_loc);
			};
			auto u16 = [&](std::shared_ptr<awst::Expression> v) {
				return awst::makeExtract(awst::makeItob(std::move(v), m_loc), 6, 2, m_loc);
			};

			auto& pre = m_ctx.preEffects();
			pre.push_back(awst::makeAssignmentStatement(bv(slotN), _a.slot, m_loc));
			pre.push_back(awst::makeAssignmentStatement(
				uv(nN), toU64(readSlotWord(bv(slotN), m_loc)), m_loc));
			pre.push_back(awst::makeAssignmentStatement(
				bv(dataN), dynDataBase(bv(slotN), m_loc), m_loc));
			pre.push_back(awst::makeAssignmentStatement(uv(iN), u64c(0), m_loc));
			if (elemDynamic)
			{
				pre.push_back(awst::makeAssignmentStatement(
					xv(headsN), awst::makeBytesConstant({}, m_loc), m_loc));
				pre.push_back(awst::makeAssignmentStatement(
					xv(tailsN), awst::makeBytesConstant({}, m_loc), m_loc));
				pre.push_back(awst::makeAssignmentStatement(
					uv(offN), awst::makeUInt64BinOp(u64c(2),
						awst::UInt64BinaryOperator::Mult, uv(nN), m_loc), m_loc));
			}
			else
				pre.push_back(awst::makeAssignmentStatement(
					xv(resultN), u16(uv(nN)), m_loc));

			auto loop = awst::makeBlock(m_loc);
			Addr child = elemAddr(bv(dataN), asBigIndex(uv(iN)), elemType);
			child.solType = elemType;
			child.wtype = m_ctx.typeMapper.map(elemType);
			auto lowered = m_ctx.lowerOperand([&]() {
				return readAny(child, elemType);
			}, true);
			for (auto& statement: lowered.effects.pre)
				loop->body.push_back(std::move(statement));
			if (!lowered.value)
				return nullptr;
			std::shared_ptr<awst::Expression> encoded;
			if (awst::structurallyEquivalent(lowered.value->wtype, elemArc4))
				encoded = awst::makeAsBytes(std::move(lowered.value), m_loc);
			else
				encoded = awst::makeAsBytes(awst::makeARC4Encode(
					std::move(lowered.value), elemArc4, m_loc), m_loc);
			loop->body.push_back(awst::makeAssignmentStatement(
				xv(innerN), std::move(encoded), m_loc));
			for (auto& statement: lowered.effects.post)
				loop->body.push_back(std::move(statement));
			if (elemDynamic)
			{
				loop->body.push_back(awst::makeAssignmentStatement(
					xv(headsN), awst::makeConcat(
						xv(headsN), u16(uv(offN)), m_loc), m_loc));
				loop->body.push_back(awst::makeAssignmentStatement(
					xv(tailsN), awst::makeConcat(
						xv(tailsN), xv(innerN), m_loc), m_loc));
				loop->body.push_back(awst::makeAssignmentStatement(
					uv(offN), awst::makeUInt64BinOp(uv(offN),
						awst::UInt64BinaryOperator::Add,
						awst::makeLen(xv(innerN), m_loc), m_loc), m_loc));
			}
			else
				loop->body.push_back(awst::makeAssignmentStatement(
					xv(resultN), awst::makeConcat(
						xv(resultN), xv(innerN), m_loc), m_loc));
			loop->body.push_back(awst::makeAssignmentStatement(
				uv(iN), awst::makeUInt64BinOp(uv(iN),
					awst::UInt64BinaryOperator::Add, u64c(1), m_loc), m_loc));
			pre.push_back(awst::makeWhileLoop(
				awst::makeNumericCompare(uv(iN), awst::NumericComparison::Lt,
					uv(nN), m_loc), std::move(loop), m_loc));
			if (elemDynamic)
				pre.push_back(awst::makeAssignmentStatement(
					xv(resultN), awst::makeConcat(u16(uv(nN)),
						awst::makeConcat(xv(headsN), xv(tailsN), m_loc), m_loc),
					m_loc));
			return awst::makeReinterpretCast(xv(resultN), arrW, m_loc);
		}
		auto call = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___evm_dynarr_recursive_read"},
			awst::WType::bytesType(), m_loc);
		awst::pushCallArg(call->args, "__slot", _a.slot);
		awst::pushCallArg(call->args, "__depth",
			awst::makeIntegerConstant(uint64_t{chain.depth}, m_loc));
		awst::pushCallArg(call->args, "__size",
			awst::makeIntegerConstant(uint64_t{chain.leafMetrics.size}, m_loc));
		awst::pushCallArg(call->args, "__aw",
			awst::makeIntegerConstant(uint64_t{chain.leafMetrics.arc4Width}, m_loc));
		awst::pushCallArg(call->args, "__per",
			awst::makeIntegerConstant(uint64_t{chain.leafMetrics.perSlot}, m_loc));
		awst::pushCallArg(call->args, "__mul",
			awst::makeIntegerConstant(uint64_t{chain.leafMetrics.lanesPerElem}, m_loc));
		awst::pushCallArg(call->args, "__bp",
			awst::makeIntegerConstant(uint64_t{chain.leafMetrics.bitPacked}, m_loc));
		return awst::makeReinterpretCast(std::move(call), arrW, m_loc);
	}
	auto lenU = _at->length();
	if (lenU == 0 || lenU > 64)
	{
		Logger::instance().error(
			"--evm-storage-layout: cannot materialise storage array of length "
			+ lenU.str() + " (unrolled reads capped at 64)", m_loc);
		return nullptr;
	}
	unsigned len = static_cast<unsigned>(lenU);
	auto const* elemType = _at->baseType();
	auto const* arrW = m_ctx.typeMapper.map(_at);
	auto arr = awst::makeNewArray(arrW, m_loc);

	std::string tmp = "__evmarr_"
		+ std::to_string(awst::NameGen::next("EvmSlotLowering.arr"));
	m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(tmp, awst::WType::biguintType(), m_loc),
		_a.slot, m_loc));
	auto baseVar = [&]() {
		return awst::makeVarExpression(tmp, awst::WType::biguintType(), m_loc);
	};

	if (auto const* structElem = dynamic_cast<StructType const*>(elemType))
	{
		auto stride = structElem->storageSize();
		for (unsigned j = 0; j < len; ++j)
		{
			Addr ea;
			ea.slot = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant((stride * j).str(), m_loc,
					awst::WType::biguintType()), m_loc);
			ea.solType = structElem;
			ea.wtype = m_ctx.typeMapper.map(structElem);
			auto ev = readStructValue(ea);
			if (!ev)
				return nullptr;
			arr->values.push_back(std::move(ev));
		}
		return arr;
	}
	// ARRAY elements recurse per element, regardless of whether the child is
	// fixed or dynamic.  Solidity's storageSize() is the correct stride in
	// both cases (a dynamic child occupies its one length/head slot), and the
	// recursive call owns the child's representation.  Keeping this branch
	// structural avoids flattening an aggregate into the scalar loop below.
	if (auto const* eat = dynamic_cast<ArrayType const*>(elemType);
		eat && !isBytesLike(elemType))
	{
		auto stride = elemType->storageSize();
		for (unsigned j = 0; j < len; ++j)
		{
			Addr ea;
			ea.slot = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant((stride * j).str(), m_loc,
					awst::WType::biguintType()), m_loc);
			ea.byteOffset = nullptr;
			ea.size = 32;
			ea.solType = elemType;
			ea.wtype = m_ctx.typeMapper.map(elemType);
			auto ev = readArrayValue(ea, eat);
			if (!ev)
				return nullptr;
			arr->values.push_back(std::move(ev));
		}
		return arr;
	}

	awst::WType const* elemW = nullptr;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(arrW))
		elemW = sa->elementType();
	if (!elemW)
		elemW = m_ctx.typeMapper.map(elemType);
	auto l = SlotHandleAccess::layoutFor(elemType);
	for (unsigned j = 0; j < len; ++j)
	{
		auto v = SlotHandleAccess::readScalarElem(
			baseVar(), awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType()),
			l, elemType, m_loc);
		if (elemW == awst::WType::arc4BoolType())
		{
			auto b = awst::makeNumericCompare(std::move(v),
				awst::NumericComparison::Ne,
				awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType()),
				m_loc);
			arr->values.push_back(awst::makeARC4Encode(std::move(b), elemW, m_loc));
		}
		else if (auto const* fbt =
			dynamic_cast<solidity::frontend::FixedBytesType const*>(elemType))
		{
			// bytesN element: the packed window's bytes ARE the value — the
			// canonical-biguint detour has no ARC4Encode into byte[N]
			// ("cannot encode biguint to uint8[2]"). Relabel the raw bytes.
			auto raw = awst::makeLeftPadToN(
				awst::makeAsBytes(std::move(v), m_loc),
				static_cast<int>(fbt->numBytes()), m_loc);
			arr->values.push_back(
				awst::makeReinterpretCast(std::move(raw), elemW, m_loc));
		}
		else
			arr->values.push_back(awst::makeARC4Encode(std::move(v), elemW, m_loc));
	}
	return arr;
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readValue(Addr const& _a)
{
	// A packed address reads both the main slot and its auxiliary high-byte
	// slot inside one expression tree.  Share a SingleEvaluation node so a
	// computed slot expression is evaluated exactly once.
	auto readSlot = (_a.wtype == awst::WType::accountType() && _a.size == 20)
		? std::shared_ptr<awst::Expression>(awst::makeEvalOnce(_a.slot, m_loc))
		: _a.slot;
	auto word = readSlotWord(readSlot, m_loc);
	if (_a.size == 32 && !_a.byteOffset)
	{
		// Fast path: a biguint carrier IS the raw word (canonical 256-bit TC);
		// only signed sub-256 needs the sign-extension step. Skipping the
		// pad/extract round-trip saves ~15 bytes of TEAL per read site.
		if (_a.wtype == awst::WType::biguintType())
			return TypeCoercion::signExtendSignedElement(
				std::move(word), _a.solType, m_loc);
		auto raw = awst::makeLeftPadToN(awst::makeAsBytes(std::move(word), m_loc), 32, m_loc);
		return SlotWordCodec::packedBytesToNative(std::move(raw), _a.wtype, _a.solType, 32, m_loc);
	}
	auto wordB = awst::makeLeftPadToN(awst::makeAsBytes(std::move(word), m_loc), 32, m_loc);
	// start = 32 - byteOffset - size (byte window, big-endian word)
	std::shared_ptr<awst::Expression> start = awst::makeIntegerConstant(
		static_cast<uint64_t>(32 - _a.size), m_loc);
	if (_a.byteOffset)
		start = awst::makeUInt64BinOp(std::move(start),
			awst::UInt64BinaryOperator::Sub, _a.byteOffset, m_loc);
	auto raw = awst::makeExtract3(std::move(wordB), std::move(start),
		awst::makeIntegerConstant(static_cast<uint64_t>(_a.size), m_loc), m_loc);
	// PACKED address: the word window holds the EVM-shaped trailing 20 bytes;
	// the high 12 live in the shadow aux slot (zeros when never written —
	// EVM-equivalent). Recombine to the full AVM address.
	if (_a.wtype == awst::WType::accountType() && _a.size == 20)
	{
		auto aux = readSlotWord(
			packedAddrAuxSlot(readSlot, _a.byteOffset, m_loc), m_loc);
		auto hi = awst::makeExtract(
			awst::makeLeftPadToN(awst::makeAsBytes(std::move(aux), m_loc), 32, m_loc),
			20, 12, m_loc);
		return awst::makeAsAccount(
			awst::makeConcat(std::move(hi), std::move(raw), m_loc), m_loc);
	}
	return SlotWordCodec::packedBytesToNative(
		std::move(raw), _a.wtype, _a.solType, _a.size, m_loc);
}

bool EvmSlotLowering::writeAny(
	Addr& _a,
	Type const* _t,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (!_t || !_value)
		return false;
	_a.solType = _t;
	if (!_a.wtype)
		_a.wtype = m_ctx.typeMapper.map(_t);
	if (_t->isValueType())
	{
		auto nat = coerceToNative(std::move(_value), _a);
		if (!nat)
			return false;
		writeValue(_a, std::move(nat), _out);
		return true;
	}
	if (isBytesLike(_t))
	{
		if (_value->wtype && _value->wtype->kind() != awst::WTypeKind::Bytes
			&& _value->wtype != awst::WType::stringType())
			_value = awst::makeARC4Decode(
				std::move(_value), bytesLikeDecodeTarget(_t), m_loc);
		writeBytesValue(_a, std::move(_value), _out);
		return true;
	}
	if (auto const* at = dynamic_cast<ArrayType const*>(_t))
		return writeArrayValue(_a, at, std::move(_value), _out);
	if (dynamic_cast<StructType const*>(_t))
		return writeStructValue(_a, std::move(_value), _out);
	return false;
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readAny(
	Addr const& _a, Type const* _t)
{
	if (!_t)
		return nullptr;
	Addr a = _a;
	a.solType = _t;
	if (!a.wtype)
		a.wtype = m_ctx.typeMapper.map(_t);
	if (_t->isValueType())
		return readValue(a);
	if (isBytesLike(_t))
		return readBytesValue(a);
	if (auto const* at = dynamic_cast<ArrayType const*>(_t))
		return readArrayValue(a, at);
	if (dynamic_cast<StructType const*>(_t))
		return readStructValue(a);
	Logger::instance().error(
		"--evm-storage-layout: declared type cannot be materialised as a value",
		m_loc);
	return nullptr;
}

bool EvmSlotLowering::clearAggregate(
	Addr const& _a,
	Type const* _t,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (!_t)
		return false;
	if (isBytesLike(_t))
	{
		writeBytesValue(_a, awst::makeBytesConstant({}, m_loc), _out);
		return true;
	}
	if (auto const* at = dynamic_cast<ArrayType const*>(_t))
	{
		if (at->isDynamicallySized())
		{
			// length 0 + old-tail clear == EVM delete semantics, and the
			// subroutine exists since S1. Aggregate elements' own keccak
			// regions (e.g. string[] entries) are NOT reachable this way.
			auto const* arrWc = m_ctx.typeMapper.map(at);
			auto const* dac = dynamic_cast<awst::ARC4DynamicArray const*>(arrWc);
			auto metc = dac ? dynElemMetrics(at->baseType(), dac->elementType())
							: DynElemMetrics{};
			if (!metc.ok)
			{
				// AGGREGATE elements. EVM's delete clears each element's own
				// region recursively — dynamic_multi_array_cleanup re-grows
				// the array afterwards and requires zeros, so leaving stale
				// data behind a zeroed length word is observably wrong.
				auto const* et = at->baseType();
				// A mapping element owns NOTHING at its own slot (content is
				// at keccak(key ++ elemSlot), which EVM cannot clear either),
				// so length-only IS the matching semantics.
				if (!dynamic_cast<solidity::frontend::MappingType const*>(et))
				{
					auto nm = [&](char const* tag) {
						return std::string("__evmclr_") + tag + "_"
							+ std::to_string(awst::NameGen::next("EvmSlotLowering.clrLoop"));
					};
					std::string ivar = nm("i"), lvar = nm("n"), dvar = nm("d");
					auto bv = [&](std::string const& n) {
						return awst::makeVarExpression(n, awst::WType::biguintType(), m_loc);
					};
					// length and data base must be read BEFORE anything is
					// zeroed — the loop needs both.
					_out.push_back(awst::makeAssignmentStatement(
						bv(lvar), readSlotWord(_a.slot, m_loc), m_loc));
					_out.push_back(awst::makeAssignmentStatement(
						bv(dvar), dynDataBase(_a.slot, m_loc), m_loc));
					_out.push_back(awst::makeAssignmentStatement(
						bv(ivar), awst::makeZero(m_loc, awst::WType::biguintType()), m_loc));
					auto stride = et->storageSize();
					Addr ea;
					ea.slot = awst::makeBigUIntBinOp(bv(dvar),
						awst::BigUIntBinaryOperator::Add,
						stride == 1
							? std::shared_ptr<awst::Expression>(bv(ivar))
							: std::shared_ptr<awst::Expression>(awst::makeBigUIntBinOp(
								bv(ivar), awst::BigUIntBinaryOperator::Mult,
								awst::makeIntegerConstant(stride.str(), m_loc,
									awst::WType::biguintType()), m_loc)),
						m_loc);
					ea.solType = et;
					ea.wtype = m_ctx.typeMapper.map(et);
					std::vector<std::shared_ptr<awst::Statement>> body;
					if (!clearAggregate(ea, et, body))
						return false;
					body.push_back(awst::makeAssignmentStatement(bv(ivar),
						awst::makeBigUIntBinOp(bv(ivar),
							awst::BigUIntBinaryOperator::Add,
							awst::makeIntegerConstant("1", m_loc,
								awst::WType::biguintType()), m_loc), m_loc));
					auto blk = awst::makeBlock(m_loc);
					blk->body = std::move(body);
					_out.push_back(awst::makeWhileLoop(
						awst::makeNumericCompare(bv(ivar),
							awst::NumericComparison::Lt, bv(lvar), m_loc),
						std::move(blk), m_loc));
				}
				_out.push_back(SlotHandleAccess::writeSlot(_a.slot,
					awst::makeZero(m_loc, awst::WType::biguintType()), m_loc));
				return true;
			}
			auto call = awst::makeSubroutineCall(
				awst::SubroutineID{"__puyasol___evm_dynarr_write"},
				awst::WType::voidType(), m_loc);
			awst::pushCallArg(call->args, "__slot", _a.slot);
			awst::pushCallArg(call->args, "__val",
				awst::makeBytesConstant({0, 0}, m_loc));
			awst::pushCallArg(call->args, "__size",
				awst::makeIntegerConstant(uint64_t{metc.size}, m_loc));
			awst::pushCallArg(call->args, "__aw",
				awst::makeIntegerConstant(uint64_t{metc.arc4Width}, m_loc));
			awst::pushCallArg(call->args, "__per",
				awst::makeIntegerConstant(uint64_t{metc.perSlot}, m_loc));
			awst::pushCallArg(call->args, "__mul",
				awst::makeIntegerConstant(uint64_t{metc.lanesPerElem}, m_loc));
			awst::pushCallArg(call->args, "__bp",
				awst::makeIntegerConstant(uint64_t{metc.bitPacked}, m_loc));
			_out.push_back(awst::makeExpressionStatement(std::move(call), m_loc));
			return true;
		}
		auto lenU = at->length();
		if (lenU == 0 || lenU > 64)
		{
			Logger::instance().error(
				"--evm-storage-layout: delete on storage array of length "
				+ lenU.str() + " not supported (cap 64)", m_loc);
			return false;
		}
		unsigned len = static_cast<unsigned>(lenU);
		auto const* elemType = at->baseType();
		// pin the base once
		std::string bs = "__evmcl_"
			+ std::to_string(awst::NameGen::next("EvmSlotLowering.clr"));
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc),
			_a.slot, m_loc));
		auto baseVar = [&]() {
			return awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc);
		};
		if (elemType->isValueType())
		{
			// the array owns its whole slot span — zero it (packed included)
			auto span = at->storageSize();
			for (solidity::u256 j = 0; j < span; ++j)
				_out.push_back(SlotHandleAccess::writeSlot(
					awst::makeBigUIntBinOp(baseVar(),
						awst::BigUIntBinaryOperator::Add,
						awst::makeIntegerConstant(j.str(), m_loc,
							awst::WType::biguintType()), m_loc),
					awst::makeZero(m_loc, awst::WType::biguintType()), m_loc));
			return true;
		}
		auto stride = elemType->storageSize();
		for (unsigned j = 0; j < len; ++j)
		{
			Addr ea;
			ea.slot = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant((stride * j).str(), m_loc,
					awst::WType::biguintType()), m_loc);
			ea.solType = elemType;
			ea.wtype = m_ctx.typeMapper.map(elemType);
			if (!clearAggregate(ea, elemType, _out))
				return false;
		}
		return true;
	}
	if (auto const* st = dynamic_cast<StructType const*>(_t))
	{
		std::string bs = "__evmcl_"
			+ std::to_string(awst::NameGen::next("EvmSlotLowering.clrS"));
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc),
			_a.slot, m_loc));
		auto baseVar = [&]() {
			return awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc);
		};
		// zero the struct's slot span (mapping-member slots hold no data, so
		// zeroing them is harmless; EVM delete skips mapping CONTENT, and so
		// do we — the keccak regions are untouched)
		auto span = st->storageSize();
		if (span > 64)
		{
			Logger::instance().error(
				"--evm-storage-layout: delete on struct spanning "
				+ span.str() + " slots not supported (cap 64)", m_loc);
			return false;
		}
		// ORDER MATTERS: recurse into dynamic members FIRST. Their regions are
		// found through their length words, which the span zeroing below
		// destroys — clearing the span first would strand the data and a later
		// re-grow would read it back.
		// dynamic members: clear their keccak-region data too
		for (auto const& m: st->structDefinition().members())
		{
			if (!m || !m->type())
				continue;
			auto const* mt = m->type();
			bool needsRegion = isBytesLike(mt)
				|| dynamic_cast<ArrayType const*>(mt)
				|| dynamic_cast<StructType const*>(mt);
			if (dynamic_cast<solidity::frontend::MappingType const*>(mt))
				continue;
			if (!needsRegion || mt->isValueType())
				continue;
			auto const& off = st->storageOffsetsOfMember(m->name());
			Addr fa;
			fa.slot = off.first == 0
				? std::shared_ptr<awst::Expression>(baseVar())
				: std::shared_ptr<awst::Expression>(awst::makeBigUIntBinOp(
					baseVar(), awst::BigUIntBinaryOperator::Add,
					awst::makeIntegerConstant(off.first.str(), m_loc,
						awst::WType::biguintType()), m_loc));
			fa.solType = mt;
			fa.wtype = m_ctx.typeMapper.map(mt);
			if (!clearAggregate(fa, mt, _out))
				return false;
		}
		for (solidity::u256 j = 0; j < span; ++j)
			_out.push_back(SlotHandleAccess::writeSlot(
				awst::makeBigUIntBinOp(baseVar(),
					awst::BigUIntBinaryOperator::Add,
					awst::makeIntegerConstant(j.str(), m_loc,
						awst::WType::biguintType()), m_loc),
				awst::makeZero(m_loc, awst::WType::biguintType()), m_loc));
		return true;
	}
	Logger::instance().error(
		"--evm-storage-layout: delete on this aggregate shape not yet "
		"supported", m_loc);
	return false;
}

bool EvmSlotLowering::writeArrayValue(
	Addr const& _a,
	ArrayType const* _at,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (!_at || !_value)
		return false;
	if (_at->isDynamicallySized())
	{
		auto const* arrWw = m_ctx.typeMapper.map(_at);
		auto chain = dynamicArrayChain(_at, arrWw);
		if (chain.depth == 0 || !chain.leafMetrics.ok)
		{
			auto const* elemType = _at->baseType();
			if (dynamic_cast<MappingType const*>(elemType))
			{
				Logger::instance().error(
					"--evm-storage-layout: whole assignment of mapping-element arrays "
					"is not defined", m_loc);
				return false;
			}
			auto const* elemArc4 = m_ctx.typeMapper.mapSolTypeToARC4(elemType);
			bool const elemDynamic = arc4IsDynamic(elemArc4);
			int const elemSize = computeEncodedElementSize(elemArc4);
			if (!elemArc4 || (!elemDynamic && elemSize <= 0))
			{
				Logger::instance().error(
					"--evm-storage-layout: array element has no representable ARC4 encoding",
					m_loc);
				return false;
			}
			if (_value->wtype != awst::WType::bytesType())
				_value = awst::makeReinterpretCast(
					std::move(_value), awst::WType::bytesType(), m_loc);

			int uid = awst::NameGen::next("EvmSlotLowering.genericArrayWrite");
			auto name = [&](char const* tag) {
				return std::string("__evm_gaw_") + tag + "_" + std::to_string(uid);
			};
			std::string slotN = name("slot"), dataN = name("base"), valN = name("val"),
				nN = name("n"), oldN = name("old"), iN = name("i"),
				startN = name("start"), endN = name("end");
			auto bv = [&](std::string const& n) {
				return awst::makeVarExpression(n, awst::WType::biguintType(), m_loc);
			};
			auto uv = [&](std::string const& n) {
				return awst::makeVarExpression(n, awst::WType::uint64Type(), m_loc);
			};
			auto xv = [&](std::string const& n) {
				return awst::makeVarExpression(n, awst::WType::bytesType(), m_loc);
			};
			auto u64c = [&](uint64_t v) {
				return awst::makeIntegerConstant(v, m_loc);
			};
			auto toU64 = [&](std::shared_ptr<awst::Expression> v) {
				return awst::makeBtoi(awst::makeExtractLastN(
					awst::makeZeroExtendToN(awst::makeAsBytes(std::move(v), m_loc),
						8, m_loc), 8, m_loc), m_loc);
			};
			auto asBigIndex = [&](std::shared_ptr<awst::Expression> v) {
				return awst::makeAsBiguint(awst::makeItob(std::move(v), m_loc), m_loc);
			};
			auto headAbs = [&](std::shared_ptr<awst::Expression> idx) {
				return awst::makeUInt64BinOp(u64c(2),
					awst::UInt64BinaryOperator::Add,
					awst::makeBtoi(awst::makeExtract3(xv(valN),
						awst::makeUInt64BinOp(u64c(2),
							awst::UInt64BinaryOperator::Add,
							awst::makeUInt64BinOp(u64c(2),
								awst::UInt64BinaryOperator::Mult,
								std::move(idx), m_loc), m_loc),
						u64c(2), m_loc), m_loc), m_loc);
			};

			_out.push_back(awst::makeAssignmentStatement(bv(slotN), _a.slot, m_loc));
			_out.push_back(awst::makeAssignmentStatement(xv(valN), std::move(_value), m_loc));
			_out.push_back(awst::makeAssignmentStatement(
				uv(oldN), toU64(readSlotWord(bv(slotN), m_loc)), m_loc));
			_out.push_back(awst::makeAssignmentStatement(
				uv(nN), awst::makeBtoi(awst::makeExtract(xv(valN), 0, 2, m_loc), m_loc),
				m_loc));
			_out.push_back(SlotHandleAccess::writeSlot(
				bv(slotN), asBigIndex(uv(nN)), m_loc));
			_out.push_back(awst::makeAssignmentStatement(
				bv(dataN), dynDataBase(bv(slotN), m_loc), m_loc));
			_out.push_back(awst::makeAssignmentStatement(uv(iN), u64c(0), m_loc));

			auto loop = awst::makeBlock(m_loc);
			std::shared_ptr<awst::Expression> childBytes;
			if (elemDynamic)
			{
				loop->body.push_back(awst::makeAssignmentStatement(
					uv(startN), headAbs(uv(iN)), m_loc));
				auto hasNext = awst::makeNumericCompare(
					awst::makeUInt64BinOp(uv(iN), awst::UInt64BinaryOperator::Add,
						u64c(1), m_loc), awst::NumericComparison::Lt, uv(nN), m_loc);
				loop->body.push_back(awst::makeAssignmentStatement(
					uv(endN), awst::makeConditional(std::move(hasNext),
						headAbs(awst::makeUInt64BinOp(uv(iN),
							awst::UInt64BinaryOperator::Add, u64c(1), m_loc)),
						awst::makeLen(xv(valN), m_loc),
						awst::WType::uint64Type(), m_loc), m_loc));
				childBytes = awst::makeExtract3(xv(valN), uv(startN),
					awst::makeUInt64BinOp(uv(endN), awst::UInt64BinaryOperator::Sub,
						uv(startN), m_loc), m_loc);
			}
			else
			{
				auto start = awst::makeUInt64BinOp(u64c(2),
					awst::UInt64BinaryOperator::Add,
					awst::makeUInt64BinOp(uv(iN),
						awst::UInt64BinaryOperator::Mult,
						u64c(static_cast<uint64_t>(elemSize)), m_loc), m_loc);
				childBytes = awst::makeExtract3(xv(valN), std::move(start),
					u64c(static_cast<uint64_t>(elemSize)), m_loc);
			}
			Addr child = elemAddr(bv(dataN), asBigIndex(uv(iN)), elemType);
			child.solType = elemType;
			child.wtype = m_ctx.typeMapper.map(elemType);
			auto childValue = awst::makeReinterpretCast(
				std::move(childBytes), elemArc4, m_loc);
			if (!writeAny(child, elemType, std::move(childValue), loop->body))
				return false;
			loop->body.push_back(awst::makeAssignmentStatement(
				uv(iN), awst::makeUInt64BinOp(uv(iN),
					awst::UInt64BinaryOperator::Add, u64c(1), m_loc), m_loc));
			_out.push_back(awst::makeWhileLoop(
				awst::makeNumericCompare(uv(iN), awst::NumericComparison::Lt,
					uv(nN), m_loc), std::move(loop), m_loc));

			// Recursively clear elements made unreachable by a shrink. Mapping
			// content is intentionally excluded above, matching Solidity delete.
			auto clearLoop = awst::makeBlock(m_loc);
			Addr oldChild = elemAddr(bv(dataN), asBigIndex(uv(iN)), elemType);
			oldChild.solType = elemType;
			oldChild.wtype = m_ctx.typeMapper.map(elemType);
			if (!clearAggregate(oldChild, elemType, clearLoop->body))
				return false;
			clearLoop->body.push_back(awst::makeAssignmentStatement(
				uv(iN), awst::makeUInt64BinOp(uv(iN),
					awst::UInt64BinaryOperator::Add, u64c(1), m_loc), m_loc));
			_out.push_back(awst::makeWhileLoop(
				awst::makeNumericCompare(uv(iN), awst::NumericComparison::Lt,
					uv(oldN), m_loc), std::move(clearLoop), m_loc));
			return true;
		}
		if (_value->wtype != awst::WType::bytesType())
			_value = awst::makeReinterpretCast(
				std::move(_value), awst::WType::bytesType(), m_loc);
		auto call = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___evm_dynarr_recursive_write"},
			awst::WType::voidType(), m_loc);
		awst::pushCallArg(call->args, "__slot", _a.slot);
		awst::pushCallArg(call->args, "__val", std::move(_value));
		awst::pushCallArg(call->args, "__depth",
			awst::makeIntegerConstant(uint64_t{chain.depth}, m_loc));
		awst::pushCallArg(call->args, "__size",
			awst::makeIntegerConstant(uint64_t{chain.leafMetrics.size}, m_loc));
		awst::pushCallArg(call->args, "__aw",
			awst::makeIntegerConstant(uint64_t{chain.leafMetrics.arc4Width}, m_loc));
		awst::pushCallArg(call->args, "__per",
			awst::makeIntegerConstant(uint64_t{chain.leafMetrics.perSlot}, m_loc));
		awst::pushCallArg(call->args, "__mul",
			awst::makeIntegerConstant(uint64_t{chain.leafMetrics.lanesPerElem}, m_loc));
		awst::pushCallArg(call->args, "__bp",
			awst::makeIntegerConstant(uint64_t{chain.leafMetrics.bitPacked}, m_loc));
		_out.push_back(awst::makeExpressionStatement(std::move(call), m_loc));
		return true;
	}

	auto lenU = _at->length();
	if (lenU == 0 || lenU > 64)
	{
		Logger::instance().error(
			"--evm-storage-layout: cannot assign storage array of length "
			+ lenU.str() + " (unrolled writes capped at 64)", m_loc);
		return false;
	}
	unsigned len = static_cast<unsigned>(lenU);
	auto const* elemType = _at->baseType();
	auto const* arrW = _value->wtype;
	awst::WType const* elemW = nullptr;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(arrW))
		elemW = sa->elementType();
	else if (auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(arrW))
		elemW = da->elementType();

	// pin base + value: both feed one statement per element
	std::string bs = "__evmaw_b_"
		+ std::to_string(awst::NameGen::next("EvmSlotLowering.arrWB"));
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc),
		_a.slot, m_loc));
	std::string vs = "__evmaw_v_"
		+ std::to_string(awst::NameGen::next("EvmSlotLowering.arrWV"));
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(vs, arrW, m_loc), std::move(_value), m_loc));
	auto baseVar = [&]() {
		return awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc);
	};
	auto valVar = [&]() {
		return awst::makeVarExpression(vs, arrW, m_loc);
	};
	// A SHORTER source zero-fills the tail, as Solidity does: `uint256[10]
	// storage x; x = [11, 12, 13]` leaves 11,12,13,0,0,0,0,0,0,0. The unroll
	// runs over the TARGET length, so without this every element past the
	// source's end indexed off its end -- `extract 3 1` on a 3-byte value --
	// and the whole assignment reverted with "index access is out of bounds",
	// silently leaving the array at its previous contents.
	unsigned srcLen = 0;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(arrW))
		srcLen = static_cast<unsigned>(sa->arraySize());
	auto const* defaultW = elemW ? elemW : m_ctx.typeMapper.map(elemType);
	auto beyondSource = [&](unsigned j) { return srcLen != 0 && j >= srcLen; };
	auto elemAt = [&](unsigned j) -> std::shared_ptr<awst::Expression> {
		if (beyondSource(j))
			return builder::TypeCoercion::makeDefaultValue(defaultW, m_loc);
		return awst::makeIndexExpression(valVar(),
			awst::makeIntegerConstant(static_cast<uint64_t>(j), m_loc),
			elemW, m_loc);
	};

	if (auto const* structElem = dynamic_cast<StructType const*>(elemType))
	{
		auto stride = structElem->storageSize();
		for (unsigned j = 0; j < len; ++j)
		{
			Addr ea;
			ea.slot = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant((stride * j).str(), m_loc,
					awst::WType::biguintType()), m_loc);
			ea.solType = structElem;
			ea.wtype = elemW;
			if (!writeStructValue(ea, elemAt(j), _out))
				return false;
		}
		return true;
	}
	if (isBytesLike(elemType))
	{
		for (unsigned j = 0; j < len; ++j)
		{
			Addr ea;
			ea.slot = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant(static_cast<uint64_t>(j), m_loc,
					awst::WType::biguintType()), m_loc);
			ea.solType = elemType;
			ea.wtype = m_ctx.typeMapper.map(elemType);
			auto ev = elemAt(j);
			std::shared_ptr<awst::Expression> fv = std::move(ev);
			if (fv->wtype && fv->wtype->kind() != awst::WTypeKind::Bytes
				&& fv->wtype != awst::WType::stringType())
				fv = awst::makeARC4Decode(std::move(fv),
					bytesLikeDecodeTarget(elemType), m_loc);
			writeBytesValue(ea, std::move(fv), _out);
		}
		return true;
	}
	// ARRAY elements: recurse per element. Fixed sub-arrays slice via arc4
	// indexing; DYNAMIC sub-arrays follow the static-array head table
	// manually (u16 offsets at position 2j, relative to the value start —
	// arc4 element-indexing does not decode head/tail for this shape).
	if (auto const* eat2 = dynamic_cast<ArrayType const*>(elemType);
		eat2 && !isBytesLike(elemType))
	{
		bool dynInner = eat2->isDynamicallySized();
		auto stride = dynInner ? solidity::u256(1) : elemType->storageSize();
		std::shared_ptr<awst::Expression> rawBytes;
		std::string rb;
		if (dynInner)
		{
			rb = "__evmaw_r_"
				+ std::to_string(awst::NameGen::next("EvmSlotLowering.arrWR"));
			_out.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(rb, awst::WType::bytesType(), m_loc),
				awst::makeReinterpretCast(valVar(), awst::WType::bytesType(),
					m_loc), m_loc));
		}
		auto rawVar = [&]() {
			return awst::makeVarExpression(rb, awst::WType::bytesType(), m_loc);
		};
		auto headAt = [&](unsigned idx) {
			return awst::makeBtoi(awst::makeExtract(rawVar(),
				static_cast<int>(2 * idx), 2, m_loc), m_loc);
		};
		for (unsigned j = 0; j < len; ++j)
		{
			Addr ea;
			ea.slot = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant((stride * j).str(), m_loc,
					awst::WType::biguintType()), m_loc);
			ea.byteOffset = nullptr;
			ea.size = 32;
			ea.solType = elemType;
			ea.wtype = elemW ? elemW : m_ctx.typeMapper.map(elemType);
			std::shared_ptr<awst::Expression> ev;
			if (beyondSource(j))
				// Same zero-fill, but the head table would be read off its end
				// too, so it cannot go through elemAt.
				ev = builder::TypeCoercion::makeDefaultValue(ea.wtype, m_loc);
			else if (dynInner)
			{
				auto start = headAt(j);
				auto end = (j + 1 < len)
					? std::shared_ptr<awst::Expression>(headAt(j + 1))
					: std::shared_ptr<awst::Expression>(
						awst::makeLen(rawVar(), m_loc));
				auto sliceLen = awst::makeUInt64BinOp(std::move(end),
					awst::UInt64BinaryOperator::Sub, start, m_loc);
				ev = awst::makeReinterpretCast(
					awst::makeExtract3(rawVar(), headAt(j),
						std::move(sliceLen), m_loc),
					ea.wtype, m_loc);
			}
			else
				ev = elemAt(j);
			if (!writeArrayValue(ea, eat2, std::move(ev), _out))
				return false;
		}
		return true;
	}
	if (!elemType->isValueType())
	{
		Logger::instance().error(
			"--evm-storage-layout: fixed array assignment with aggregate "
			"elements of this shape not yet supported", m_loc);
		return false;
	}
	auto l = SlotHandleAccess::layoutFor(elemType);
	auto const* nativeW = m_ctx.typeMapper.map(elemType);
	for (unsigned j = 0; j < len; ++j)
	{
		auto ev = elemAt(j);
		std::shared_ptr<awst::Expression> nat = std::move(ev);
		if (nat->wtype != nativeW)
			nat = awst::makeARC4Decode(std::move(nat), nativeW, m_loc);
		auto canonical = awst::makeAsBiguint(
			SlotWordCodec::nativeToPackedBytes(std::move(nat), nativeW, 32, m_loc),
			m_loc);
		SlotHandleAccess::writeScalarElem(_out, baseVar(),
			awst::makeIntegerConstant(static_cast<uint64_t>(j), m_loc,
				awst::WType::biguintType()),
			l, std::move(canonical), m_loc);
	}
	return true;
}

void EvmSlotLowering::writeValue(
	Addr const& _a,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	// The value's carrier can lag the slot's declared wtype (storing
	// `new helper()` leaves an APPLICATION where the contract-typed state
	// var maps to account — 15 slot-lane fixtures died in puya on
	// asBytes(application)). The canonical conversions already live in
	// coerceForAssignment; route through them once, up front.
	if (_value && _a.wtype && _value->wtype != _a.wtype
		&& _value->wtype != awst::WType::biguintType())
		_value = TypeCoercion::coerceForAssignment(std::move(_value), _a.wtype, m_loc);
	if (_a.size == 32 && !_a.byteOffset)
	{
		// Fast path: a biguint value is already the canonical word.
		if (_value && _value->wtype == awst::WType::biguintType())
		{
			_out.push_back(SlotHandleAccess::writeSlot(
				_a.slot, std::move(_value), m_loc));
			return;
		}
		auto packed = SlotWordCodec::nativeToPackedBytes(
			std::move(_value), _a.wtype, 32, m_loc);
		_out.push_back(SlotHandleAccess::writeSlot(
			_a.slot, awst::makeAsBiguint(std::move(packed), m_loc), m_loc));
		return;
	}
	// PACKED address: stash the high 12 bytes in the shadow aux slot (the
	// word window keeps the EVM-shaped trailing 20 for asm fidelity). Both
	// the value and slot feed TWO statements, so pin each to a named temp;
	// SingleEvaluation does not persist across statement boundaries.
	std::shared_ptr<awst::Expression> slotOnce;
	if (_a.wtype == awst::WType::accountType() && _a.size == 20)
	{
		std::string slotName = "__evm_addr_slot_"
			+ std::to_string(awst::NameGen::next("EvmSlotLowering.addrSlotPin"));
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(
				slotName, awst::WType::biguintType(), m_loc),
			_a.slot, m_loc));
		slotOnce = awst::makeVarExpression(
			slotName, awst::WType::biguintType(), m_loc);
		std::string nm = "__evm_addr_"
			+ std::to_string(awst::NameGen::next("EvmSlotLowering.addrPin"));
		auto const* pinW = _value->wtype ? _value->wtype : _a.wtype;
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(nm, pinW, m_loc), std::move(_value), m_loc));
		_value = awst::makeVarExpression(nm, pinW, m_loc);
		auto hi = awst::makeExtract(
			awst::makeAsBytes(awst::makeVarExpression(nm, pinW, m_loc), m_loc),
			0, 12, m_loc);
		_out.push_back(SlotHandleAccess::writeSlot(
			packedAddrAuxSlot(slotOnce, _a.byteOffset, m_loc),
			awst::makeAsBiguint(std::move(hi), m_loc), m_loc));
	}
	else
	{
		// For every other sub-word write both uses remain inside the final
		// write statement, where EvalOnce is the appropriate sharing scope.
		slotOnce = awst::makeEvalOnce(_a.slot, m_loc);
	}
	auto packed = SlotWordCodec::nativeToPackedBytes(
		std::move(_value), _a.wtype, _a.size, m_loc);
	auto wordB = awst::makeLeftPadToN(
		awst::makeAsBytes(readSlotWord(slotOnce, m_loc), m_loc), 32, m_loc);
	std::shared_ptr<awst::Expression> start = awst::makeIntegerConstant(
		static_cast<uint64_t>(32 - _a.size), m_loc);
	if (_a.byteOffset)
		start = awst::makeUInt64BinOp(std::move(start),
			awst::UInt64BinaryOperator::Sub, _a.byteOffset, m_loc);
	auto newWord = awst::makeReplace3(
		std::move(wordB), std::move(start), std::move(packed), m_loc);
	_out.push_back(SlotHandleAccess::writeSlot(
		slotOnce, awst::makeAsBiguint(std::move(newWord), m_loc), m_loc));
}

} // namespace puyasol::builder::sol_ast
