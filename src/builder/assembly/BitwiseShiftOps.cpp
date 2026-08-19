/// @file BitwiseShiftOps.cpp
/// Bitwise and shift operations: shl, shr, div, byte, signextend, buildPowerOf2.

#include "builder/assembly/AssemblyBuilder.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"
#include "builder/proxies/Erc1967Lowering.h"
#include "builder/BuildArtifacts.h"
#include "awst/NameGen.h"

#include <boost/multiprecision/cpp_int.hpp>

#include <sstream>

namespace puyasol::builder
{


namespace
{
/// Box-length tuple pieces for a named array box: (count, exists) where
/// count = (box_len - 2) / 32 (ARC4 dynamic array: 2-byte header + 32B elems).
struct BoxCount
{
	std::shared_ptr<awst::Expression> count;   // uint64 (guard with exists!)
	std::shared_ptr<awst::Expression> exists;  // bool
};
BoxCount makeArrayBoxCount(std::string const& _name, awst::SourceLocation const& _loc)
{
	auto u64c = [&](uint64_t v) { return awst::makeIntegerConstant(v, _loc); };
	static awst::WTuple s_boxLenTupleType(std::vector<awst::WType const*>{
		awst::WType::uint64Type(), awst::WType::boolType()});
	auto lenTuple = awst::makeBoxLen(
		awst::makeUtf8BytesConstant(_name, _loc), &s_boxLenTupleType, _loc);
	auto len64 = awst::makeTupleItem(lenTuple, 0, awst::WType::uint64Type(), _loc);
	auto exists = awst::makeTupleItem(lenTuple, 1, awst::WType::boolType(), _loc);
	// (len - 2) / 32 — only meaningful when exists (guarded by callers).
	auto count = awst::makeUInt64BinOp(
		awst::makeUInt64BinOp(std::move(len64), awst::UInt64BinaryOperator::Sub, u64c(2), _loc),
		awst::UInt64BinaryOperator::FloorDiv, u64c(32), _loc);
	return {std::move(count), std::move(exists)};
}
} // namespace

std::shared_ptr<awst::Expression> AssemblyBuilder::tryRouteConstSlotLoad(
	std::shared_ptr<awst::Expression> const& _slot,
	awst::SourceLocation const& _loc)
{
	auto const* ic = dynamic_cast<awst::IntegerConstant const*>(_slot.get());
	if (!ic)
		return nullptr;
	auto u64c = [&](uint64_t v) { return awst::makeIntegerConstant(v, _loc); };

	auto it = m_slotRoutes.find(ic->value);
	if (it != m_slotRoutes.end())
	{
		auto const& r = it->second;
		if (r.kind == SlotRoute::Kind::Scalar)
		{
			// The var's app-global, padded/truncated to the 32-byte slot word.
			auto get = awst::makeIntrinsicCall("app_global_get", awst::WType::bytesType(), _loc);
			get->stackArgs.push_back(awst::makeUtf8BytesConstant(r.varName, _loc));
			return awst::makeAsBiguint(awst::makeExtractLastN(
				awst::makeLeftPad(std::move(get), 32, _loc), 32, _loc), _loc);
		}
		if (r.kind == SlotRoute::Kind::ArrayRoot)
		{
			// EVM: a dynamic array's root slot holds its LENGTH.
			auto bc = makeArrayBoxCount(r.varName, _loc);
			auto lenOrZero = awst::makeConditional(
				std::move(bc.exists), std::move(bc.count), u64c(0),
				awst::WType::uint64Type(), _loc);
			return awst::makeAsBiguint(awst::makeItob(std::move(lenOrZero), _loc), _loc);
		}
		if (r.kind == SlotRoute::Kind::StructMemberArrayRoot)
		{
			// Root slot of a dyn array INSIDE a struct box: length = the ARC4
			// dynamic array's uint16 count prefix within the struct's encoding.
			auto const* st = dynamic_cast<awst::ARC4Struct const*>(r.wtype);
			awst::WType const* fieldType = nullptr;
			if (st)
				for (auto const& [fname, ftype]: st->fields())
					if (fname == r.fieldName) { fieldType = ftype; break; }
			if (!fieldType)
				return nullptr;
			auto box = StorageMapper::makeTopLevelBoxExpr(r.varName, r.wtype, _loc);
			auto readBase = StorageMapper::makeStateGetWithDefault(box, r.wtype, _loc);
			auto field = awst::makeFieldExpression(readBase, r.fieldName, fieldType, _loc);
			auto len16 = awst::makeIntrinsicCall("extract_uint16", awst::WType::uint64Type(), _loc);
			len16->stackArgs.push_back(awst::makeAsBytes(std::move(field), _loc));
			len16->stackArgs.push_back(u64c(0));
			return awst::makeAsBiguint(awst::makeItob(std::move(len16), _loc), _loc);
		}
	}

	// Data regions: slot in [K, K + 2^32) reads element (slot - K).
	boost::multiprecision::cpp_int slot(ic->value);
	for (auto const& reg: m_slotDataRegions)
	{
		boost::multiprecision::cpp_int base(reg.dataBase);
		if (slot < base || slot - base >= (boost::multiprecision::cpp_int(1) << 32))
			continue;
		uint64_t idx = static_cast<uint64_t>(slot - base);
		auto bc = makeArrayBoxCount(reg.varName, _loc);
		// exists && idx < count → element bytes; else 0 (EVM: popped/beyond-length
		// slots read zero — pop clears, and our box physically shrinks).
		auto inRange = awst::makeBoolBinOp(
			std::move(bc.exists), awst::BinaryBooleanOperator::And,
			awst::makeNumericCompare(u64c(idx), awst::NumericComparison::Lt,
				std::move(bc.count), _loc), _loc);
		auto elem = awst::makeAsBiguint(awst::makeBoxExtract(
			awst::makeUtf8BytesConstant(reg.varName, _loc),
			u64c(2 + idx * 32), u64c(32), _loc), _loc);
		return awst::makeConditional(std::move(inRange), std::move(elem),
			awst::makeBiguintConstant("0", _loc), awst::WType::biguintType(), _loc);
	}
	return nullptr;
}

bool AssemblyBuilder::tryRouteConstSlotStore(
	std::shared_ptr<awst::Expression> const& _slot,
	std::shared_ptr<awst::Expression> const& _value,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	auto const* ic = dynamic_cast<awst::IntegerConstant const*>(_slot.get());
	if (!ic)
		return false;
	auto u64c = [&](uint64_t v) { return awst::makeIntegerConstant(v, _loc); };
	auto nameBytes = [&](std::string const& n) { return awst::makeUtf8BytesConstant(n, _loc); };

	auto it = m_slotRoutes.find(ic->value);
	if (it != m_slotRoutes.end())
	{
		auto const& r = it->second;
		if (r.kind == SlotRoute::Kind::Scalar)
		{
			auto key = awst::makeUtf8BytesConstant(r.varName, _loc, awst::WType::stateKeyType());
			auto target = awst::makeAppStateExpression(std::move(key), r.wtype, _loc);
			auto assign = awst::makeAssignmentExpression(
				std::move(target), ensureBiguint(_value, _loc), _loc, r.wtype);
			_out.push_back(awst::makeExpressionStatement(std::move(assign), _loc));
			return true;
		}
		if (r.kind == SlotRoute::Kind::ArrayRoot)
		{
			// sstore(root, L) = SET LENGTH: resize the backing box to 2 + L*32
			// (box_resize zero-fills growth — matching push()-style zeroing) and
			// stamp the 2-byte ARC4 count header. box_create when absent
			// (box_create errors if a box exists at a different size).
			auto newLen = safeBtoi(ensureBiguint(_value, _loc), _loc);
			// bind once: used in size math + header stamp
			std::string tmp = "__sslot_len_" + std::to_string(awst::NameGen::next("AssemblyBuilder.sslotLen"));
			_out.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(tmp, awst::WType::uint64Type(), _loc),
				std::move(newLen), _loc));
			auto lenVar = [&]() { return awst::makeVarExpression(tmp, awst::WType::uint64Type(), _loc); };
			auto newSize = [&]() {
				return awst::makeUInt64BinOp(u64c(2), awst::UInt64BinaryOperator::Add,
					awst::makeUInt64BinOp(lenVar(), awst::UInt64BinaryOperator::Mult, u64c(32), _loc), _loc);
			};

			auto bc = makeArrayBoxCount(r.varName, _loc);
			auto resizeBlk = awst::makeBlock(_loc);
			{
				auto resize = awst::makeIntrinsicCall("box_resize", awst::WType::voidType(), _loc);
				resize->stackArgs.push_back(nameBytes(r.varName));
				resize->stackArgs.push_back(newSize());
				resizeBlk->body.push_back(awst::makeExpressionStatement(std::move(resize), _loc));
			}
			auto createBlk = awst::makeBlock(_loc);
			{
				auto create = awst::makeBoxCreate(nameBytes(r.varName), newSize(), _loc);
				createBlk->body.push_back(awst::makeExpressionStatement(std::move(create), _loc));
			}
			_out.push_back(awst::makeIfElse(
				std::move(bc.exists), std::move(resizeBlk), std::move(createBlk), _loc));

			// header = 2-byte BE count
			auto hdr = awst::makeExtract3(
				awst::makeItob(lenVar(), _loc), u64c(6), u64c(2), _loc);
			auto put = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), _loc);
			put->stackArgs.push_back(nameBytes(r.varName));
			put->stackArgs.push_back(u64c(0));
			put->stackArgs.push_back(std::move(hdr));
			_out.push_back(awst::makeExpressionStatement(std::move(put), _loc));
			return true;
		}
		if (r.kind == SlotRoute::Kind::StructMemberArrayRoot)
		{
			// sstore(memberRoot, L) = SET LENGTH of a dyn array living INSIDE a
			// struct box: COW-rebuild the struct with the member replaced by a
			// zero-filled length-L array (2-byte BE count ++ L*32 zero bytes) —
			// EVM length-grow exposes zeroed slots, so fresh zeros match.
			auto const* st = dynamic_cast<awst::ARC4Struct const*>(r.wtype);
			awst::WType const* fieldType = nullptr;
			if (st)
				for (auto const& [fname, ftype]: st->fields())
					if (fname == r.fieldName) { fieldType = ftype; break; }
			if (!fieldType)
				return false;

			auto newLen = safeBtoi(ensureBiguint(_value, _loc), _loc);
			// bind once: used in header stamp + zero-fill size
			std::string tmp = "__sslot_len_" + std::to_string(awst::NameGen::next("AssemblyBuilder.sslotLen"));
			_out.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(tmp, awst::WType::uint64Type(), _loc),
				std::move(newLen), _loc));
			auto lenVar = [&]() { return awst::makeVarExpression(tmp, awst::WType::uint64Type(), _loc); };

			auto hdr = awst::makeExtract3(
				awst::makeItob(lenVar(), _loc), u64c(6), u64c(2), _loc);
			auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
			zeros->stackArgs.push_back(awst::makeUInt64BinOp(
				lenVar(), awst::UInt64BinaryOperator::Mult, u64c(32), _loc));
			auto fieldVal = awst::makeReinterpretCast(
				awst::makeConcat(std::move(hdr), std::move(zeros), _loc), fieldType, _loc);

			auto box = StorageMapper::makeTopLevelBoxExpr(r.varName, r.wtype, _loc);
			auto readBase = StorageMapper::makeStateGetWithDefault(box, r.wtype, _loc);
			auto newStruct = awst::makeStructWithReplacedField(
				st, readBase, r.fieldName, std::move(fieldVal), _loc);
			_out.push_back(awst::makeAssignmentStatement(
				StorageMapper::makeTopLevelBoxExpr(r.varName, r.wtype, _loc),
				std::move(newStruct), _loc));
			return true;
		}
	}

	boost::multiprecision::cpp_int slot(ic->value);
	for (auto const& reg: m_slotDataRegions)
	{
		boost::multiprecision::cpp_int base(reg.dataBase);
		if (slot < base || slot - base >= (boost::multiprecision::cpp_int(1) << 32))
			continue;
		uint64_t idx = static_cast<uint64_t>(slot - base);
		auto bc = makeArrayBoxCount(reg.varName, _loc);
		auto inRange = awst::makeBoolBinOp(
			std::move(bc.exists), awst::BinaryBooleanOperator::And,
			awst::makeNumericCompare(u64c(idx), awst::NumericComparison::Lt,
				std::move(bc.count), _loc), _loc);
		auto thenBlk = awst::makeBlock(_loc);
		{
			auto put = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), _loc);
			put->stackArgs.push_back(nameBytes(reg.varName));
			put->stackArgs.push_back(u64c(2 + idx * 32));
			put->stackArgs.push_back(awst::makeLeftPadToN(
				awst::makeAsBytes(ensureBiguint(_value, _loc), _loc), 32, _loc));
			thenBlk->body.push_back(awst::makeExpressionStatement(std::move(put), _loc));
		}
		auto elseBlk = awst::makeBlock(_loc);
		{
			// Beyond current length: EVM keeps the raw write invisible until a
			// length-grow — the box-per-slot fallback preserves the bits.
			auto call = awst::makeSubroutineCall(
				awst::SubroutineID{"__puyasol___storage_write"}, awst::WType::voidType(), _loc);
			awst::pushCallArg(call->args, "__slot", ensureBiguint(_slot, _loc));
			awst::pushCallArg(call->args, "__value", ensureBiguint(_value, _loc));
			elseBlk->body.push_back(awst::makeExpressionStatement(std::move(call), _loc));
		}
		_out.push_back(awst::makeIfElse(
			std::move(inRange), std::move(thenBlk), std::move(elseBlk), _loc));
		return true;
	}
	return false;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 1, "sload", _loc))
		return nullptr;

	// EIP-1967 proxy slots (proxy.md §1): admin → synthesized global,
	// implementation → this app's own identity, beacon → runtime trap.
	switch (proxies::Erc1967Lowering::classify(_args[0].get()))
	{
	case proxies::Erc1967Slot::Admin:
		m_typeMapper.artifacts().noteErc1967AdminUse();
		return proxies::Erc1967Lowering::adminLoad(_loc);
	case proxies::Erc1967Slot::Implementation:
		return proxies::Erc1967Lowering::implementationLoad(_loc);
	case proxies::Erc1967Slot::Beacon:
		Logger::instance().warning(
			"ERC-1967 beacon slot read lowers to a runtime failure — this call "
			"site REVERTS if ever reached (see proxy.md)", _loc);
		m_pendingStatements.push_back(proxies::Erc1967Lowering::trapStatement(
			proxies::Erc1967Slot::Beacon, /*_isStore=*/false, _loc));
		return awst::makeBiguintConstant("0", _loc);
	case proxies::Erc1967Slot::None:
		break;
	}

	// CONSTANT slot → route directly to the named variable's storage (scalar
	// global / array length / array element). See SlotRoute.
	if (auto routed = tryRouteConstSlotLoad(_args[0], _loc))
		return routed;

	// Box-keyed ARC4 struct slot sentinel (`sload(s.slot)` where `s` is a struct
	// storage-ref param/alias): read the EVM slot-0 packed word from the box.
	if (auto box = std::dynamic_pointer_cast<awst::BoxValueExpression>(_args[0]))
		if (dynamic_cast<awst::ARC4Struct const*>(box->wtype))
			return handleBoxKeyedStructSlotLoad(box, _loc);

	// Full-width slot: __storage_read takes the 256-bit slot (no truncation).
	auto slotArg = ensureBiguintSlotArg(_args[0], _loc);

	auto call = awst::makeSubroutineCall(awst::SubroutineID{"__puyasol___storage_read"}, awst::WType::biguintType(), _loc);

	awst::pushCallArg(call->args, "__slot", std::move(slotArg));

	return call;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleGas(
	awst::SourceLocation const& _loc
)
{
	// Returns uint64; consumer coerces via ensureBiguint (match at consumption, drops itob widen).
	EvmFeaturePolicy::report(
		EvmFeature::GasLeft, m_typeMapper.profile(), _loc);
	return awst::makeGlobal(std::string("OpcodeBudget"), awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleTimestamp(
	awst::SourceLocation const& _loc
)
{
	// Returns uint64; consumer coerces (same natural-type convention as gas/number/selfbalance).
	return awst::makeGlobal("LatestTimestamp", awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::buildPowerOf2(
	std::shared_ptr<awst::Expression> _shift,
	awst::SourceLocation const& _loc
)
{
	// 2^shift via setbit(bzero(32), 255-shift, 1) — AVM has no bexp opcode.
	auto shiftAmt = _shift;

	if (shiftAmt->wtype != awst::WType::uint64Type())
	{
		auto cast = awst::makeAsBytes(std::move(shiftAmt), _loc);
		auto cat = awst::makeLeftPad(std::move(cast), 8, _loc);
		auto extract = awst::makeExtractLastN(std::move(cat), 8, _loc);
		shiftAmt = awst::makeBtoi(std::move(extract), _loc);
	}

	auto bzero = awst::makeBzero(32, _loc);

	// Clamp to 0..255: puya optimizer may strip wrapMod256 from intermediates.
	auto clampedShift = awst::makeUInt64BinOp(std::move(shiftAmt), awst::UInt64BinaryOperator::Mod,
		awst::makeIntegerConstant("256", _loc), _loc);

	// setbit uses MSB-first: bit (255-shift) == 2^shift.
	auto bitIdx = awst::makeUInt64BinOp(
		awst::makeIntegerConstant("255", _loc),
		awst::UInt64BinaryOperator::Sub, std::move(clampedShift), _loc);

	return awst::makeAsBiguint(
		awst::makeSetbit(std::move(bzero), std::move(bitIdx), awst::makeOne(_loc), _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleDiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 2, "div", _loc))
		return nullptr;
	// EVM div(a,0)=0; AVM panics.
	return safeDivMod(
		_args[0], awst::BigUIntBinaryOperator::FloorDiv, _args[1], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleShl(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// shl(shift, value) → (value * 2^shift) mod 2^256, or 0 when shift ≥ 256 (EIP-145).
	// Arg order is (shift, value) — reversed vs. C/AVM.
	if (!checkArity(_args, 2, "shl", _loc))
		return nullptr;
	// Materialize once: shift feeds both buildPowerOf2 and the <256 conditional
	// (makeEvalOnce = OperandPlan primitive; skips SE on a constant amount).
	auto shift = awst::makeEvalOnce(ensureBiguint(_args[0], _loc), _loc);
	// Reduce value mod 2^256 before multiplying. Negative int128 decoded as
	// arc4.uint512 yields a 512-bit biguint; (512-bit * 2^s) overflows AVM's
	// 64-byte bigint limit before the trailing mod can reduce it (V4 Pool.updateTick
	// shl(128, liquidityNet)). Pre-reduction is a no-op for values already <2^256.
	auto value = wrapMod256(ensureBiguint(_args[1], _loc), _loc);
	auto power = buildPowerOf2(shift, _loc);
	auto product = makeBigUIntBinOp(
		value, awst::BigUIntBinaryOperator::Mult, std::move(power), _loc
	);
	auto wrapped = wrapMod256(std::move(product), _loc);
	auto twoFiftySix = awst::makeIntegerConstant(
		"256", _loc, awst::WType::biguintType());
	auto cond = awst::makeNumericCompare(
		shift, awst::NumericComparison::Lt, std::move(twoFiftySix), _loc);
	auto zero = awst::makeIntegerConstant(
		"0", _loc, awst::WType::biguintType());
	return awst::makeConditional(
		std::move(cond), std::move(wrapped), std::move(zero),
		awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleShr(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// shr(shift, value) → value / 2^shift, or 0 when shift ≥ 256 (EIP-145).
	// `shr(sub(256,…), MASK)` → shr(256,…) at bucket boundaries must return 0
	// (see AAVE PositionStatusMap.sol:223). AVM b>> has no such clamping.
	if (!checkArity(_args, 2, "shr", _loc))
		return nullptr;
	// Materialize once: shift feeds both buildPowerOf2 and the <256 conditional
	// (makeEvalOnce = OperandPlan primitive; skips SE on a constant amount).
	auto shift = awst::makeEvalOnce(ensureBiguint(_args[0], _loc), _loc);
	auto value = _args[1];
	auto power = buildPowerOf2(shift, _loc);
	auto divResult = makeBigUIntBinOp(
		value, awst::BigUIntBinaryOperator::FloorDiv, std::move(power), _loc
	);
	auto twoFiftySix = awst::makeIntegerConstant(
		"256", _loc, awst::WType::biguintType());
	auto cond = awst::makeNumericCompare(
		shift, awst::NumericComparison::Lt, std::move(twoFiftySix), _loc);
	auto zero = awst::makeIntegerConstant(
		"0", _loc, awst::WType::biguintType());
	return awst::makeConditional(
		std::move(cond), std::move(divResult), std::move(zero),
		awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleByte(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// byte(n, x): extract byte n from 32-byte big-endian x → biguint.
	if (!checkArity(_args, 2, "byte", _loc))
		return nullptr;

	auto padded = padTo32Bytes(_args[1], _loc);

	// n >= 32: EVM byte() returns 0 (out of range). Range-check the ORIGINAL n as a
	// biguint — checking the btoi-truncated value is wrong: a huge n (>= 2^64)
	// truncates to a small in-range index and wrongly extracts a byte (found
	// fuzzing Solady DateTimeLib.daysInMonth: byte(2^128+5, ...) returned 31, not
	// 0). The conditional only evaluates the extract on the taken branch (n < 32),
	// so the btoi used there is always in range and never OOB-reverts.
	auto nBig = awst::makeEvalOnce(ensureBiguint(_args[0], _loc), _loc);
	auto inRange = awst::makeNumericCompare(
		nBig, awst::NumericComparison::Lt,
		awst::makeIntegerConstant("32", _loc, awst::WType::biguintType()), _loc);
	auto nU64 = safeBtoi(nBig, _loc);
	auto extracted = awst::makeAsBiguint(
		awst::makeExtract3(std::move(padded), std::move(nU64), awst::makeOne(_loc), _loc), _loc);
	return awst::makeConditional(
		std::move(inRange), std::move(extracted),
		awst::makeBiguintConstant("0", _loc), awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSignextend(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// signextend(b, x): sign-extend x from byte b (0-indexed from low byte).
	// Identity: signextend(b,x) == sar(s, shl(s, x)), s = 248-8*min(b,31).
	// shl moves byte b's sign bit to bit 255; sar replicates it down.
	if (!checkArity(_args, 2, "signextend", _loc))
		return nullptr;

	auto x = ensureBiguint(_args[1], _loc);

	// STRICT literal check, NOT resolveConstantOffset: that helper recurses into
	// ABI-decode expressions and would fold a runtime `b` to the selector offset (4).
	std::optional<uint64_t> bConst;
	if (auto* lit = dynamic_cast<awst::IntegerConstant*>(_args[0].get()))
		bConst = std::stoull(lit->value);
	if (!bConst.has_value())
	{
		// Runtime b: s = 248 - 8*min(b,31). min(b,31) gives b≥31 no-op for free
		// (s=0 → sar(0,shl(0,x))=x) and prevents 248-8*b from underflowing.
		auto biguint = awst::WType::biguintType();
		auto buildS = [&]() -> std::shared_ptr<awst::Expression> {
			auto cmp = awst::makeNumericCompare(
				ensureBiguint(_args[0], _loc), awst::NumericComparison::Lt,
				awst::makeIntegerConstant("31", _loc, biguint), _loc);
			auto bc = awst::makeConditional(
				std::move(cmp), ensureBiguint(_args[0], _loc),
				awst::makeIntegerConstant("31", _loc, biguint), biguint, _loc);
			auto eightBc = makeBigUIntBinOp(
				std::move(bc), awst::BigUIntBinaryOperator::Mult,
				awst::makeIntegerConstant("8", _loc, biguint), _loc);
			return makeBigUIntBinOp(
				awst::makeIntegerConstant("248", _loc, biguint),
				awst::BigUIntBinaryOperator::Sub, std::move(eightBc), _loc);
		};
		std::vector<std::shared_ptr<awst::Expression>> shlArgs{
			buildS(), ensureBiguint(_args[1], _loc)};
		auto shifted = handleShl(shlArgs, _loc);
		std::vector<std::shared_ptr<awst::Expression>> sarArgs{
			buildS(), std::move(shifted)};
		return handleSar(sarArgs, _loc);
	}

	uint64_t b = *bConst;
	if (b >= 31)
		return x; // signextend from byte ≥31 is a no-op for 256-bit values

	// Same sar(s,shl(s,x)) lowering as the runtime path (on-chain verified).
	// The earlier conditional-mask form re-read x three times and derived the
	// sign bit from the full x, breaking V4 LiquidityMath.addDelta's
	// `add(and(x,mask), signextend(15, y))` for negative deltas
	// (test: inlineAssembly/signextend_adddelta).
	uint64_t s = 248 - 8 * b;
	auto sStr = std::to_string(s);
	std::vector<std::shared_ptr<awst::Expression>> shlArgs{
		awst::makeIntegerConstant(sStr, _loc, awst::WType::biguintType()), std::move(x)};
	auto shifted = handleShl(shlArgs, _loc);
	std::vector<std::shared_ptr<awst::Expression>> sarArgs{
		awst::makeIntegerConstant(sStr, _loc, awst::WType::biguintType()), std::move(shifted)};
	return handleSar(sarArgs, _loc);
}


} // namespace puyasol::builder
