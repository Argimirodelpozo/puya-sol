/// @file SimpleSplitter.cpp
/// Static "extract-named-subroutines" splitter — see SimpleSplitter.h.

#include "splitter/SimpleSplitter.h"
#include "Logger.h"
#include "awst/WType.h"

#include <deque>
#include <memory>
#include <set>
#include <vector>

namespace puyasol::splitter
{

namespace
{

constexpr int TxnTypeAppl = 6;

// ABI type-name string for a WType — must match what puya emits for the
// helper's method signature so sha512_256("name(args)return")[:4] aligns.
std::string abiTypeName(awst::WType const* t)
{
	if (!t) return "void";
	if (t == awst::WType::voidType()) return "void";
	if (t == awst::WType::boolType()) return "bool";
	if (t == awst::WType::uint64Type()) return "uint64";
	if (t == awst::WType::biguintType()) return "uint512";
	if (t == awst::WType::accountType()) return "address";
	if (t == awst::WType::stringType()) return "string";
	if (t->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bw = dynamic_cast<awst::BytesWType const*>(t);
		if (bw && bw->length()) return "byte[" + std::to_string(*bw->length()) + "]";
		return "byte[]";
	}
	if (t->kind() == awst::WTypeKind::ARC4UIntN)
	{
		auto const* a = dynamic_cast<awst::ARC4UIntN const*>(t);
		if (a)
		{
			if (a->arc4Alias() == "byte") return "byte";
			return "uint" + std::to_string(a->n());
		}
	}
	if (t->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		auto const* a = dynamic_cast<awst::ARC4StaticArray const*>(t);
		if (a) return abiTypeName(a->elementType()) + "[" + std::to_string(a->arraySize()) + "]";
	}
	if (t->kind() == awst::WTypeKind::ARC4DynamicArray)
	{
		auto const* a = dynamic_cast<awst::ARC4DynamicArray const*>(t);
		if (a)
		{
			// puya treats "string" / "byte[]" / "address" aliases as the
			// canonical ABI typename. Match that exactly so the selector
			// hashes line up with the helper's emitted method signature.
			auto const& alias = a->arc4Alias();
			// puya's canonical aliases must match exactly so selector hashes align.
			if (alias == "string" || alias == "byte[]" || alias == "address") return alias;
			return abiTypeName(a->elementType()) + "[]";
		}
	}
	if (t->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* s = dynamic_cast<awst::ARC4Struct const*>(t);
		if (s)
		{
			std::string out = "(";
			bool first = true;
			for (auto const& f : s->fields())
			{
				if (!first) out += ",";
				out += abiTypeName(f.second);
				first = false;
			}
			out += ")";
			return out;
		}
	}
	if (t->kind() == awst::WTypeKind::ARC4Tuple)
	{
		auto const* tup = dynamic_cast<awst::ARC4Tuple const*>(t);
		if (tup)
		{
			std::string out = "(";
			bool first = true;
			for (auto const* el : tup->types())
			{
				if (!first) out += ",";
				out += abiTypeName(el);
				first = false;
			}
			out += ")";
			return out;
		}
	}
	if (t->kind() == awst::WTypeKind::WTuple)
	{
		auto const* tup = dynamic_cast<awst::WTuple const*>(t);
		if (tup)
		{
			std::string out = "(";
			bool first = true;
			for (auto const* el : tup->types())
			{
				if (!first) out += ",";
				out += abiTypeName(el);
				first = false;
			}
			out += ")";
			return out;
		}
	}
	return "byte[]";  // fallthrough; puya may reject
}

std::string buildMethodSig(std::string const& name, awst::Subroutine const& sub)
{
	std::string sig = name + "(";
	bool first = true;
	for (auto const& a : sub.args)
	{
		if (!first) sig += ",";
		sig += abiTypeName(a.wtype);
		first = false;
	}
	sig += ")";
	sig += abiTypeName(sub.returnType);
	return sig;
}

/// Encode one argument for ApplicationArgs (ABI-encoded bytes).
std::shared_ptr<awst::Expression> encodeArg(
	std::shared_ptr<awst::Expression> argExpr,
	awst::SourceLocation const& loc)
{
	auto const* wt = argExpr->wtype;
	// Dynamic bytes needs a uint16 length prefix (arc4.dynamic_array<arc4.uint8>)
	// or the helper's ARC4 router `len; ==; assert` decode check fails.
	if (wt && wt->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bw = dynamic_cast<awst::BytesWType const*>(wt);
		bool isDynamic = bw && !bw->length().has_value();
		if (isDynamic)
		{
			auto raw = awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), loc);
			// Build uint16 length-prefix: itob(len) → extract last 2 bytes.
			auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), loc);
			lenCall->stackArgs.push_back(raw);
			auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), loc);
			itob->stackArgs.push_back(std::move(lenCall));
			auto lenPrefix = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), loc);
			lenPrefix->immediates.push_back(6);
			lenPrefix->immediates.push_back(2);
			lenPrefix->stackArgs.push_back(std::move(itob));
			// concat: prefix ++ raw
			auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
			concat->stackArgs.push_back(std::move(lenPrefix));
			concat->stackArgs.push_back(std::move(raw));
			return concat;
		}
		return awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), loc);
	}
	if (wt == awst::WType::accountType())
	{
		return awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), loc);
	}
	if (wt == awst::WType::biguintType())
	{
		// Pad biguint to 64 bytes (uint512) — puya auto-asserts `len == 64` at
		// the router; runtime biguint can be <64 bytes so OR-pad to extend.
		auto bytesArg = awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), loc);
		auto bzero = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), loc);
		bzero->stackArgs.push_back(awst::makeIntegerConstant("64", loc));
		auto orOp = awst::makeIntrinsicCall("b|", awst::WType::bytesType(), loc);
		orOp->stackArgs.push_back(std::move(bzero));
		orOp->stackArgs.push_back(std::move(bytesArg));
		return orOp;
	}
	if (wt == awst::WType::uint64Type())
	{
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), loc);
		itob->stackArgs.push_back(std::move(argExpr));
		return itob;
	}
	if (wt == awst::WType::boolType())
	{
		// arc4.bool: 1-byte buffer, top bit = the bool.
		auto base = awst::makeBytesConstant({0x00}, loc, awst::BytesEncoding::Base16);
		auto setbit = awst::makeIntrinsicCall("setbit", awst::WType::bytesType(), loc);
		setbit->stackArgs.push_back(std::move(base));
		setbit->stackArgs.push_back(awst::makeIntegerConstant("0", loc));
		auto val = awst::makeReinterpretCast(std::move(argExpr), awst::WType::uint64Type(), loc);
		setbit->stackArgs.push_back(std::move(val));
		return setbit;
	}
	// ARC4 composite types: in-memory rep IS the ARC4 bytes — reinterpret.
	if (wt && (wt->kind() == awst::WTypeKind::ARC4Struct
		|| wt->kind() == awst::WTypeKind::ARC4StaticArray
		|| wt->kind() == awst::WTypeKind::ARC4DynamicArray
		|| wt->kind() == awst::WTypeKind::ARC4Tuple
		|| wt->kind() == awst::WTypeKind::ARC4UIntN
		|| wt->kind() == awst::WTypeKind::ARC4UFixedNxM))
	{
		return awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), loc);
	}
	// WTuple: flatten to ABI bytes via ARC4Encode.
	if (wt && wt->kind() == awst::WTypeKind::WTuple)
	{
		auto enc = std::make_shared<awst::ARC4Encode>();
		enc->sourceLocation = loc;
		// puya derives the encoding from the inner expression's wtype.
		enc->wtype = awst::WType::bytesType();
		enc->value = std::move(argExpr);
		return awst::makeReinterpretCast(std::move(enc), awst::WType::bytesType(), loc);
	}
	return awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), loc);
}

/// Map WType to its ARC4 equivalent for building the bytes-shape ARC4Decode
/// consumes. Returns nullptr if unsupported. WTuple recursion mints fresh
/// ARC4Tuples owned by a static arena (freed at process exit).
awst::WType const* mapToArc4(awst::WType const* w)
{
	if (!w) return nullptr;
	// biguint maps to uint512 (not 256) — must match abiTypeName + the wire width
	// so ARC4Decode emits `extract 4 64` not `extract 4 4` on the return side.
	if (w == awst::WType::biguintType())
	{
		static awst::ARC4UIntN s_uint512(512);
		return &s_uint512;
	}
	if (w == awst::WType::uint64Type())
	{
		static awst::ARC4UIntN s_uint64(64);
		return &s_uint64;
	}
	if (w == awst::WType::boolType())
	{
		return awst::WType::arc4BoolType();
	}
	if (w == awst::WType::accountType())
	{
		// AVM address = 32 bytes; ARC4 wire shape = uint256.
		static awst::ARC4UIntN s_uint256(256);
		return &s_uint256;
	}
	if (w == awst::WType::stringType())
	{
		// Match puya's arc4_string_alias ("string") so the decode router accepts it.
		static awst::ARC4UIntN s_arc4Byte(8, "byte");
		static awst::ARC4DynamicArray s_arc4String(&s_arc4Byte, "string");
		return &s_arc4String;
	}
	if (awst::isDynamicBytes(w))
	{
		static awst::ARC4UIntN s_arc4Byte(8, "byte");
		static awst::ARC4DynamicArray s_dynBytes(&s_arc4Byte, "arc4.dynamic_bytes");
		return &s_dynBytes;
	}
	auto k = w->kind();
	if (k == awst::WTypeKind::ARC4Struct
		|| k == awst::WTypeKind::ARC4StaticArray
		|| k == awst::WTypeKind::ARC4DynamicArray
		|| k == awst::WTypeKind::ARC4Tuple
		|| k == awst::WTypeKind::ARC4UIntN
		|| k == awst::WTypeKind::ARC4UFixedNxM)
	{
		return w;  // identity for ARC4 types
	}
	if (k == awst::WTypeKind::WTuple)
	{
		auto const* tup = dynamic_cast<awst::WTuple const*>(w);
		std::vector<awst::WType const*> inner;
		for (auto const* el : tup->types())
		{
			auto const* m = mapToArc4(el);
			if (!m) return nullptr;
			inner.push_back(m);
		}
		static std::vector<std::unique_ptr<awst::ARC4Tuple>> s_owned;
		s_owned.push_back(std::make_unique<awst::ARC4Tuple>(std::move(inner)));
		return s_owned.back().get();
	}
	// string / bytes / unknown — caller falls back to skipping the extraction.
	return nullptr;
}

/// Decode the post-prefix LastLog bytes back to the original return type.
std::shared_ptr<awst::Expression> decodeReturn(
	std::shared_ptr<awst::Expression> bytesExpr,
	awst::WType const* retType,
	awst::SourceLocation const& loc)
{
	if (retType == awst::WType::voidType())
		return bytesExpr;
	if (retType == awst::WType::boolType())
	{
		auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), loc);
		getbit->stackArgs.push_back(std::move(bytesExpr));
		getbit->stackArgs.push_back(awst::makeIntegerConstant("0", loc));
		return awst::makeNumericCompare(
			std::move(getbit), awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", loc), loc);
	}
	if (retType == awst::WType::uint64Type())
	{
		auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), loc);
		btoi->stackArgs.push_back(std::move(bytesExpr));
		return btoi;
	}
	if (retType == awst::WType::biguintType())
	{
		// Wire is 64-byte uint512; narrow to 32-byte uint256 (Solidity max).
		// Without narrowing, downstream `len <= 32 assert overflow` fires.
		auto extracted = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), loc);
		extracted->immediates = {32, 32};  // extract bytes [32, 64)
		extracted->stackArgs.push_back(std::move(bytesExpr));
		return awst::makeReinterpretCast(std::move(extracted), retType, loc);
	}
	if (retType && retType->kind() == awst::WTypeKind::WTuple)
	{
		// Extract per-slot from the flat wire bytes so biguint slots can be
		// narrowed 64→32 bytes (uint512→uint256). ARC4Decode would leave them
		// 64-wide, triggering downstream `len <= 32 assert overflow`.
		// Only taken when every element is a fixed scalar (biguint/uint64/
		// account/bool); dynamic-element tuples fall back to ARC4Decode.
		auto const* tup = dynamic_cast<awst::WTuple const*>(retType);
		if (tup)
		{
			auto isFixedScalar = [](awst::WType const* el) {
				return el == awst::WType::biguintType()
					|| el == awst::WType::uint64Type()
					|| el == awst::WType::accountType()
					|| el == awst::WType::boolType();
			};
			bool allFixedScalar = !tup->types().empty();
			for (auto const* el : tup->types())
				if (!isFixedScalar(el)) { allFixedScalar = false; break; }
			if (allFixedScalar)
			{
				auto fixedSlotBytes = [](awst::WType const* el) -> int {
					if (el == awst::WType::biguintType()) return 64;  // uint512 wire
					if (el == awst::WType::uint64Type()) return 8;    // uint64
					if (el == awst::WType::accountType()) return 32;  // uint256
					if (el == awst::WType::boolType()) return 1;      // arc4.bool
					return 0;
				};
				auto tupleExpr = std::make_shared<awst::TupleExpression>();
				tupleExpr->sourceLocation = loc;
				tupleExpr->wtype = retType;
				int offset = 0;
				for (auto const* el : tup->types())
				{
					int slotLen = fixedSlotBytes(el);
					int readOffset = offset;
					int readLen = slotLen;
					// biguint slot is 64-byte uint512; narrow to low-32 uint256.
					if (el == awst::WType::biguintType())
					{
						readOffset = offset + 32;
						readLen = 32;
					}
					auto slice = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), loc);
					slice->immediates = {readOffset, readLen};
					slice->stackArgs.push_back(bytesExpr);
					std::shared_ptr<awst::Expression> decoded;
					if (el == awst::WType::uint64Type())
					{
						// bytes → uint64 needs btoi; ReinterpretCast rejects
						// it because the underlying scalar types differ
						// (bytes vs uint64).
						auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), loc);
						btoi->stackArgs.push_back(std::move(slice));
						decoded = std::move(btoi);
					}
					else if (el == awst::WType::boolType())
					{
						// arc4.bool is 1 byte; the high bit carries the value.
						auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), loc);
						getbit->stackArgs.push_back(std::move(slice));
						getbit->stackArgs.push_back(awst::makeIntegerConstant("0", loc));
						decoded = awst::makeNumericCompare(
							std::move(getbit), awst::NumericComparison::Ne,
							awst::makeIntegerConstant("0", loc), loc);
					}
					else
					{
						decoded = awst::makeReinterpretCast(std::move(slice), el, loc);
					}
					tupleExpr->items.push_back(std::move(decoded));
					offset += slotLen;
				}
				return tupleExpr;
			}
		}

		// Fallback for dynamic-element tuples: reinterpret+ARC4Decode to WTuple.
		auto const* arc4Form = mapToArc4(retType);
		if (!arc4Form) return awst::makeReinterpretCast(std::move(bytesExpr), retType, loc);
		auto cast = awst::makeReinterpretCast(std::move(bytesExpr), arc4Form, loc);
		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = loc;
		decode->wtype = retType;
		decode->value = std::move(cast);
		return decode;
	}
	return awst::makeReinterpretCast(std::move(bytesExpr), retType, loc);
}

/// Build a stub Block: inner-app-call to the helper + decode + return.
std::shared_ptr<awst::Block> buildStubBody(
	awst::Subroutine const& sub,
	std::string const& helperContractName)
{
	auto loc = sub.sourceLocation;
	auto block = std::make_shared<awst::Block>();
	block->sourceLocation = loc;

	// ApplicationArgs tuple: [methodSelector, encodedArg0, ...]
	auto argsTuple = std::make_shared<awst::TupleExpression>();
	argsTuple->sourceLocation = loc;

	auto methodConst = std::make_shared<awst::MethodConstant>();
	methodConst->sourceLocation = loc;
	methodConst->wtype = awst::WType::bytesType();
	methodConst->value = buildMethodSig(sub.name, sub);
	argsTuple->items.push_back(methodConst);

	std::vector<awst::WType const*> tupleTypes;
	tupleTypes.push_back(awst::WType::bytesType());
	for (auto const& a : sub.args)
	{
		auto var = awst::makeVarExpression(a.name, a.wtype, loc);
		argsTuple->items.push_back(encodeArg(std::move(var), loc));
		tupleTypes.push_back(awst::WType::bytesType());
	}
	// Owned arena — one WTuple per stub, freed at process exit.
	static std::vector<std::unique_ptr<awst::WTuple>> s_ownedStubTuples;
	s_ownedStubTuples.push_back(
		std::make_unique<awst::WTuple>(std::move(tupleTypes), std::nullopt));
	argsTuple->wtype = s_ownedStubTuples.back().get();

	// ApplicationID = TemplateVar(`TMPL_<helperName>_APP_ID`).
	auto tvar = std::make_shared<awst::TemplateVar>();
	tvar->sourceLocation = loc;
	tvar->wtype = awst::WType::uint64Type();
	tvar->name = "TMPL_" + helperContractName + "_APP_ID";
	auto appId = awst::makeReinterpretCast(
		std::move(tvar), awst::WType::applicationType(), loc);

	static awst::WInnerTransactionFields s_applFieldsType(TxnTypeAppl);
	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);

	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = loc;
	create->wtype = &s_applFieldsType;
	create->fields["TypeEnum"] = awst::makeIntegerConstant(std::to_string(TxnTypeAppl), loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", loc);
	create->fields["ApplicationID"] = std::move(appId);
	create->fields["OnCompletion"] = awst::makeIntegerConstant("0", loc);
	create->fields["ApplicationArgs"] = std::move(argsTuple);

	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = loc;
	submit->wtype = &s_applTxnType;
	submit->itxns.push_back(std::move(create));
	block->body.push_back(awst::makeExpressionStatement(std::move(submit), loc));

	// Read itxn LastLog and strip 4-byte ARC4 magic prefix.
	auto readLog = awst::makeIntrinsicCall("itxn", awst::WType::bytesType(), loc);
	readLog->immediates = {std::string("LastLog")};
	auto stripPrefix = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), loc);
	stripPrefix->immediates = {4, 0};
	stripPrefix->stackArgs.push_back(std::move(readLog));

	// Decode + return.
	auto retVal = decodeReturn(std::move(stripPrefix), sub.returnType, loc);
	if (sub.returnType == awst::WType::voidType())
	{
		block->body.push_back(awst::makeExpressionStatement(std::move(retVal), loc));
		block->body.push_back(awst::makeReturnStatement(nullptr, loc));
	}
	else
	{
		block->body.push_back(awst::makeReturnStatement(std::move(retVal), loc));
	}
	return block;
}

/// Collect SubroutineID target IDs referenced in an expression/statement tree.
void collectSubroutineIds(awst::Expression const& e, std::set<std::string>& out);
void collectSubroutineIds(awst::Statement const& s, std::set<std::string>& out);

void collectSubroutineIds(awst::Expression const& e, std::set<std::string>& out)
{
	if (auto const* sce = dynamic_cast<awst::SubroutineCallExpression const*>(&e))
	{
		if (auto const* id = std::get_if<awst::SubroutineID>(&sce->target))
			out.insert(id->target);
		// InstanceMethodTarget refs need the "memberName:" prefix so callers
		// route them to the method-lookup path (not the subroutine table).
		if (auto const* m = std::get_if<awst::InstanceMethodTarget>(&sce->target))
			out.insert(std::string("memberName:") + m->memberName);
		if (auto const* sm = std::get_if<awst::InstanceSuperMethodTarget>(&sce->target))
			out.insert(std::string("memberName:") + sm->memberName);
		for (auto const& a : sce->args)
			if (a.value) collectSubroutineIds(*a.value, out);
		return;
	}
	if (auto const* ic = dynamic_cast<awst::IntrinsicCall const*>(&e))
	{
		for (auto const& sa : ic->stackArgs)
			if (sa) collectSubroutineIds(*sa, out);
		return;
	}
	if (auto const* rc = dynamic_cast<awst::ReinterpretCast const*>(&e))
	{
		if (rc->expr) collectSubroutineIds(*rc->expr, out);
		return;
	}
	if (auto const* cmp = dynamic_cast<awst::NumericComparisonExpression const*>(&e))
	{
		if (cmp->lhs) collectSubroutineIds(*cmp->lhs, out);
		if (cmp->rhs) collectSubroutineIds(*cmp->rhs, out);
		return;
	}
	if (auto const* bcmp = dynamic_cast<awst::BytesComparisonExpression const*>(&e))
	{
		if (bcmp->lhs) collectSubroutineIds(*bcmp->lhs, out);
		if (bcmp->rhs) collectSubroutineIds(*bcmp->rhs, out);
		return;
	}
	if (auto const* bbo = dynamic_cast<awst::BooleanBinaryOperation const*>(&e))
	{
		if (bbo->left) collectSubroutineIds(*bbo->left, out);
		if (bbo->right) collectSubroutineIds(*bbo->right, out);
		return;
	}
	if (auto const* big = dynamic_cast<awst::BigUIntBinaryOperation const*>(&e))
	{
		if (big->left) collectSubroutineIds(*big->left, out);
		if (big->right) collectSubroutineIds(*big->right, out);
		return;
	}
	if (auto const* u64 = dynamic_cast<awst::UInt64BinaryOperation const*>(&e))
	{
		if (u64->left) collectSubroutineIds(*u64->left, out);
		if (u64->right) collectSubroutineIds(*u64->right, out);
		return;
	}
	if (auto const* notExpr = dynamic_cast<awst::Not const*>(&e))
	{
		if (notExpr->expr) collectSubroutineIds(*notExpr->expr, out);
		return;
	}
	if (auto const* ce = dynamic_cast<awst::ConditionalExpression const*>(&e))
	{
		if (ce->condition) collectSubroutineIds(*ce->condition, out);
		if (ce->trueExpr) collectSubroutineIds(*ce->trueExpr, out);
		if (ce->falseExpr) collectSubroutineIds(*ce->falseExpr, out);
		return;
	}
	if (auto const* ae = dynamic_cast<awst::AssignmentExpression const*>(&e))
	{
		if (ae->target) collectSubroutineIds(*ae->target, out);
		if (ae->value) collectSubroutineIds(*ae->value, out);
		return;
	}
	if (auto const* fe = dynamic_cast<awst::FieldExpression const*>(&e))
	{
		if (fe->base) collectSubroutineIds(*fe->base, out);
		return;
	}
	if (auto const* ie = dynamic_cast<awst::IndexExpression const*>(&e))
	{
		if (ie->base) collectSubroutineIds(*ie->base, out);
		if (ie->index) collectSubroutineIds(*ie->index, out);
		return;
	}
	if (auto const* te = dynamic_cast<awst::TupleExpression const*>(&e))
	{
		for (auto const& it : te->items)
			if (it) collectSubroutineIds(*it, out);
		return;
	}
	if (auto const* tie = dynamic_cast<awst::TupleItemExpression const*>(&e))
	{
		if (tie->base) collectSubroutineIds(*tie->base, out);
		return;
	}
	if (auto const* ae = dynamic_cast<awst::AssertExpression const*>(&e))
	{
		if (ae->condition) collectSubroutineIds(*ae->condition, out);
		return;
	}
	if (auto const* se = dynamic_cast<awst::SingleEvaluation const*>(&e))
	{
		if (se->source) collectSubroutineIds(*se->source, out);
		return;
	}
	if (auto const* cit = dynamic_cast<awst::CreateInnerTransaction const*>(&e))
	{
		for (auto const& [k, v] : cit->fields)
			if (v) collectSubroutineIds(*v, out);
		return;
	}
	if (auto const* sit = dynamic_cast<awst::SubmitInnerTransaction const*>(&e))
	{
		for (auto const& it : sit->itxns)
			if (it) collectSubroutineIds(*it, out);
		return;
	}
	// Other expression kinds: no subroutine refs to find.
}

void collectSubroutineIds(awst::Statement const& s, std::set<std::string>& out)
{
	if (auto const* es = dynamic_cast<awst::ExpressionStatement const*>(&s))
	{
		if (es->expr) collectSubroutineIds(*es->expr, out);
		return;
	}
	if (auto const* as = dynamic_cast<awst::AssignmentStatement const*>(&s))
	{
		if (as->target) collectSubroutineIds(*as->target, out);
		if (as->value) collectSubroutineIds(*as->value, out);
		return;
	}
	if (auto const* rs = dynamic_cast<awst::ReturnStatement const*>(&s))
	{
		if (rs->value) collectSubroutineIds(*rs->value, out);
		return;
	}
	if (auto const* b = dynamic_cast<awst::Block const*>(&s))
	{
		for (auto const& st : b->body)
			if (st) collectSubroutineIds(*st, out);
		return;
	}
	if (auto const* ie = dynamic_cast<awst::IfElse const*>(&s))
	{
		if (ie->condition) collectSubroutineIds(*ie->condition, out);
		if (ie->ifBranch) collectSubroutineIds(*ie->ifBranch, out);
		if (ie->elseBranch) collectSubroutineIds(*ie->elseBranch, out);
		return;
	}
	if (auto const* wl = dynamic_cast<awst::WhileLoop const*>(&s))
	{
		if (wl->condition) collectSubroutineIds(*wl->condition, out);
		if (wl->loopBody) collectSubroutineIds(*wl->loopBody, out);
		return;
	}
	if (auto const* sw = dynamic_cast<awst::Switch const*>(&s))
	{
		if (sw->value) collectSubroutineIds(*sw->value, out);
		for (auto const& [k, body] : sw->cases)
		{
			if (k) collectSubroutineIds(*k, out);
			if (body) collectSubroutineIds(*body, out);
		}
		if (sw->defaultCase) collectSubroutineIds(*sw->defaultCase, out);
		return;
	}
	if (auto const* em = dynamic_cast<awst::Emit const*>(&s))
	{
		if (em->value) collectSubroutineIds(*em->value, out);
		return;
	}
}

/// Transitive closure of subroutines called from `seeds`, limited to `subById`.
std::vector<std::shared_ptr<awst::Subroutine>> collectTransitiveDeps(
	std::vector<std::shared_ptr<awst::Subroutine>> const& seeds,
	std::map<std::string, std::shared_ptr<awst::Subroutine>> const& subById)
{
	std::set<std::string> seen;
	std::vector<std::shared_ptr<awst::Subroutine>> out;
	std::vector<std::shared_ptr<awst::Subroutine>> work = seeds;
	for (auto const& s : seeds) seen.insert(s->id);

	while (!work.empty())
	{
		auto cur = std::move(work.back());
		work.pop_back();
		out.push_back(cur);
		if (!cur->body) continue;
		std::set<std::string> refs;
		collectSubroutineIds(*cur->body, refs);
		for (auto const& id : refs)
		{
			if (seen.count(id)) continue;
			auto it = subById.find(id);
			if (it == subById.end()) continue;
			seen.insert(id);
			work.push_back(it->second);
		}
	}
	return out;
}

/// Build a helper Contract that exposes each sub as an ABI method.
std::shared_ptr<awst::Contract> buildHelperContract(
	awst::Contract const& original,
	std::vector<std::shared_ptr<awst::Subroutine>> const& subs,
	int helperIdx,
	std::map<std::string, uint64_t> const& ensureBudget)
{
	auto helper = std::make_shared<awst::Contract>();
	helper->id = original.id + "__Helper" + std::to_string(helperIdx);
	helper->name = original.name + "__Helper" + std::to_string(helperIdx);
	helper->methodResolutionOrder = {helper->id};
	helper->avmVersion = original.avmVersion;

	for (auto const& sub : subs)
	{
		awst::ContractMethod m;
		m.sourceLocation = sub->sourceLocation;
		m.cref = helper->id;
		m.memberName = sub->name;
		m.args = sub->args;
		m.returnType = sub->returnType;

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = sub->sourceLocation;

		// Prepend ensure_budget if --ensure-budget targets this method.
		// Match full name ("CTHelpers.getCollectionId") then bare name fallback.
		uint64_t budgetForFunc = 0;
		if (auto it = ensureBudget.find(sub->name); it != ensureBudget.end())
			budgetForFunc = it->second;
		else if (auto dot = sub->name.rfind('.'); dot != std::string::npos)
			if (auto it2 = ensureBudget.find(sub->name.substr(dot + 1));
				it2 != ensureBudget.end())
				budgetForFunc = it2->second;

		if (budgetForFunc > 0)
		{
			auto budgetVal = awst::makeIntegerConstant(
				std::to_string(budgetForFunc), sub->sourceLocation);
			auto feeSource = awst::makeIntegerConstant("0", sub->sourceLocation);
			auto ebCall = std::make_shared<awst::PuyaLibCall>();
			ebCall->sourceLocation = sub->sourceLocation;
			ebCall->wtype = awst::WType::voidType();
			ebCall->func = "ensure_budget";
			ebCall->args = {
				awst::CallArg{std::string("required_budget"), std::move(budgetVal)},
				awst::CallArg{std::string("fee_source"), std::move(feeSource)},
			};
			body->body.push_back(awst::makeExpressionStatement(
				std::move(ebCall), sub->sourceLocation));
		}

		auto callExpr = std::make_shared<awst::SubroutineCallExpression>();
		callExpr->sourceLocation = sub->sourceLocation;
		callExpr->wtype = sub->returnType;
		awst::SubroutineID target;
		target.target = sub->id;
		callExpr->target = target;
		for (auto const& a : sub->args)
		{
			awst::CallArg ca;
			ca.value = awst::makeVarExpression(a.name, a.wtype, sub->sourceLocation);
			callExpr->args.push_back(std::move(ca));
		}

		if (sub->returnType == awst::WType::voidType())
		{
			body->body.push_back(awst::makeExpressionStatement(
				std::move(callExpr), sub->sourceLocation));
			body->body.push_back(awst::makeReturnStatement(nullptr, sub->sourceLocation));
		}
		else
		{
			body->body.push_back(awst::makeReturnStatement(
				std::move(callExpr), sub->sourceLocation));
		}
		m.body = std::move(body);

		awst::ARC4ABIMethodConfig abiConfig;
		abiConfig.sourceLocation = sub->sourceLocation;
		abiConfig.allowedCompletionTypes = {0};
		abiConfig.create = 3;
		abiConfig.name = sub->name;
		abiConfig.readonly = sub->pure;
		m.arc4MethodConfig = abiConfig;

		helper->methods.push_back(std::move(m));
	}

	// approval_program: standard puya pattern —
	//   if Txn.ApplicationID == 0: return true   // allow creation
	//   else: return ARC4Router()                // dispatch on methods
	{
		auto loc = original.sourceLocation;
		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		// `if (Txn.ApplicationID == 0) return true;`
		auto appId = awst::makeIntrinsicCall("txn", awst::WType::uint64Type(), loc);
		appId->immediates = {std::string("ApplicationID")};
		auto isCreate = awst::makeNumericCompare(
			std::move(appId), awst::NumericComparison::Eq,
			awst::makeIntegerConstant("0", loc), loc);
		auto createBranch = std::make_shared<awst::Block>();
		createBranch->sourceLocation = loc;
		createBranch->body.push_back(awst::makeReturnStatement(
			awst::makeBoolConstant(true, loc), loc));
		auto ifStmt = std::make_shared<awst::IfElse>();
		ifStmt->sourceLocation = loc;
		ifStmt->condition = std::move(isCreate);
		ifStmt->ifBranch = std::move(createBranch);
		body->body.push_back(std::move(ifStmt));

		// `return ARC4Router()`
		auto routerExpr = std::make_shared<awst::ARC4Router>();
		routerExpr->sourceLocation = loc;
		routerExpr->wtype = awst::WType::boolType();
		body->body.push_back(awst::makeReturnStatement(std::move(routerExpr), loc));

		helper->approvalProgram.sourceLocation = loc;
		helper->approvalProgram.cref = helper->id;
		helper->approvalProgram.memberName = "__puya_arc4_router__";
		helper->approvalProgram.returnType = awst::WType::boolType();
		helper->approvalProgram.body = std::move(body);
	}

	// clear_program: always approve.
	{
		auto loc = original.sourceLocation;
		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;
		body->body.push_back(awst::makeReturnStatement(
			awst::makeBoolConstant(true, loc), loc));

		helper->clearProgram.sourceLocation = loc;
		helper->clearProgram.cref = helper->id;
		helper->clearProgram.memberName = "clear_state_program";
		helper->clearProgram.returnType = awst::WType::boolType();
		helper->clearProgram.body = std::move(body);
	}

	return helper;
}

} // namespace

std::vector<SimpleSplitter::ContractAWST> SimpleSplitter::split(
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots,
	std::vector<std::string> const& _moveNames,
	int _helperIndex,
	std::map<std::string, uint64_t> const& _ensureBudget)
{
	std::vector<ContractAWST> out;

	std::shared_ptr<awst::Contract> primary;
	std::vector<std::shared_ptr<awst::Subroutine>> moved;
	std::set<std::string> moveSet(_moveNames.begin(), _moveNames.end());

	// ARC4 composite types round-trip as reinterpret-casts; WTuple needs
	// ARC4Encode on the way out (handled by encodeArg).
	auto isUnsupported = [](awst::WType const* t) {
		// No arg kinds are unsupported yet; rejected separately for returns.
		(void)t;
		return false;
	};
	// WTuple returns unsupported if mapToArc4 can't express the element types.
	auto unsupportedReturn = [](awst::WType const* t) {
		if (!t) return false;
		if (t->kind() != awst::WTypeKind::WTuple) return false;
		return mapToArc4(t) == nullptr;
	};

	for (auto const& r : _roots)
	{
		if (auto c = std::dynamic_pointer_cast<awst::Contract>(r))
		{
			if (!primary) primary = c;
		}
		else if (auto s = std::dynamic_pointer_cast<awst::Subroutine>(r))
		{
			if (!moveSet.count(s->name)) continue;
			// Skip subs with undecodable return types.
			if (unsupportedReturn(s->returnType))
			{
				Logger::instance().warning(
					"SimpleSplitter: skipping '" + s->name +
					"' — WTuple return not supported yet (need ARC4Decode chain)");
				continue;
			}
			bool argSkip = false;
			for (auto const& a : s->args)
				if (isUnsupported(a.wtype)) { argSkip = true; break; }
			if (argSkip)
			{
				Logger::instance().warning(
					"SimpleSplitter: skipping '" + s->name + "'");
				continue;
			}
			moved.push_back(s);
		}
	}

	// Match ContractMethods on the primary contract's methods table. Same
	// composite-type filter as for subroutines.
	std::vector<awst::ContractMethod> movedMethods;
	std::set<std::string> movedMethodNames;
	if (primary)
	{
		for (auto const& m : primary->methods)
		{
			if (!moveSet.count(m.memberName)) continue;
			if (unsupportedReturn(m.returnType))
			{
				Logger::instance().warning(
					"SimpleSplitter: skipping method '" + m.memberName +
					"' — WTuple return not supported yet");
				continue;
			}
			bool argSkip = false;
			for (auto const& a : m.args)
				if (isUnsupported(a.wtype)) { argSkip = true; break; }
			if (argSkip)
			{
				Logger::instance().warning(
					"SimpleSplitter: skipping method '" + m.memberName + "'");
				continue;
			}
			movedMethods.push_back(m);
			movedMethodNames.insert(m.memberName);
		}
	}

	if (!primary || (moved.empty() && movedMethods.empty())) return out;

	// Set of moved subroutine names (got moved + filtered).
	std::set<std::string> movedSubNames;
	for (auto const& s : moved) movedSubNames.insert(s->name);

	// Build sub-id index so we can collect transitive deps.
	std::map<std::string, std::shared_ptr<awst::Subroutine>> subById;
	for (auto const& r : _roots)
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(r))
			subById[sub->id] = sub;

	auto helperRoots = collectTransitiveDeps(moved, subById);
	// Also pull subs referenced by moved ContractMethods.
	{
		std::set<std::string> seenIds;
		for (auto const& s : helperRoots) seenIds.insert(s->id);
		std::set<std::string> refs;
		for (auto const& m : movedMethods)
			if (m.body) collectSubroutineIds(*m.body, refs);
		std::vector<std::shared_ptr<awst::Subroutine>> seeds;
		for (auto const& id : refs)
		{
			if (id.rfind("memberName:", 0) == 0) continue;  // method ref, handled below
			if (seenIds.count(id)) continue;
			auto it = subById.find(id);
			if (it != subById.end()) seeds.push_back(it->second);
		}
		auto extra = collectTransitiveDeps(seeds, subById);
		for (auto const& s : extra)
			if (!seenIds.count(s->id))
			{
				seenIds.insert(s->id);
				helperRoots.push_back(s);
			}
	}

	// Walk InstanceMethodTarget deps (e.g. `this._matchOrders(...)`) and
	// co-locate the transitive method closure in the helper. Added to
	// movedMethods but NOT movedMethodNames — orch keeps its original bodies
	// for these (stubbing tuple-returning internal methods tripped WTuple
	// decode before). Transitively-pulled methods have no arc4MethodConfig
	// so puya emits them as internal subroutines, not ABI routes.
	if (primary)
	{
		std::map<std::string, awst::ContractMethod const*> methodByMember;
		for (auto const& m : primary->methods)
			methodByMember[m.memberName] = &m;
		std::set<std::string> seenMembers;
		for (auto const& m : movedMethods) seenMembers.insert(m.memberName);
		std::set<std::string> seenSubIds;
		for (auto const& s : helperRoots) seenSubIds.insert(s->id);

		std::deque<std::string> work;
		for (auto const& m : movedMethods) work.push_back(m.memberName);

		while (!work.empty())
		{
			auto curName = std::move(work.front());
			work.pop_front();
			auto it = methodByMember.find(curName);
			if (it == methodByMember.end() || !it->second->body) continue;

			std::set<std::string> refs;
			collectSubroutineIds(*it->second->body, refs);
			for (auto const& id : refs)
			{
				if (id.rfind("memberName:", 0) == 0)
				{
					std::string member = id.substr(11);
					if (seenMembers.count(member)) continue;
					seenMembers.insert(member);
					auto it2 = methodByMember.find(member);
					if (it2 != methodByMember.end())
					{
						// Not inlined: 50+ methods would exceed 8192 bytes.
						// arc4MethodConfig left empty → internal subroutine only.
						auto copy = *it2->second;
						copy.inlineOpt = false;
						movedMethods.push_back(std::move(copy));
						work.push_back(member);
					}
				}
				else
				{
					if (seenSubIds.count(id)) continue;
					auto sit = subById.find(id);
					if (sit == subById.end()) { seenSubIds.insert(id); continue; }
					std::vector<std::shared_ptr<awst::Subroutine>> seeds{sit->second};
					auto extra = collectTransitiveDeps(seeds, subById);
					for (auto const& s : extra)
						if (seenSubIds.insert(s->id).second)
							helperRoots.push_back(s);
				}
			}
		}
	}

	// Helper contract first (so test harness can deploy it before orchestrator
	// reads its app id from a template var).
	auto helper = buildHelperContract(*primary, moved, _helperIndex, _ensureBudget);

	// Inject __delegate_update on the helper: if helper bytes run on the orch
	// mid-dance, the revert UpdateApplication hits the helper's router, which
	// must admit OC=UpdateApplication via this selector.
	{
		auto loc = helper->sourceLocation;
		awst::ContractMethod hatch;
		hatch.cref = helper->id;
		hatch.memberName = "__delegate_update";
		hatch.returnType = awst::WType::voidType();
		hatch.sourceLocation = loc;
		auto block = std::make_shared<awst::Block>();
		block->sourceLocation = loc;
		hatch.body = std::move(block);
		awst::ARC4ABIMethodConfig abiCfg;
		abiCfg.sourceLocation = loc;
		abiCfg.allowedCompletionTypes = {4}; // UpdateApplication
		abiCfg.create = 3;
		abiCfg.name = "__delegate_update";
		hatch.arc4MethodConfig = abiCfg;
		helper->methods.push_back(std::move(hatch));
	}

	// Add moved ContractMethods to the helper. Named methods get ABI shells;
	// transitively-pulled methods are internal-only (no arc4MethodConfig).
	for (auto m : movedMethods)
	{
		m.cref = helper->id;
		if (movedMethodNames.count(m.memberName))
		{
			awst::ARC4ABIMethodConfig abiCfg;
			abiCfg.sourceLocation = m.sourceLocation;
			abiCfg.allowedCompletionTypes = {0};
			abiCfg.create = 3;
			abiCfg.name = m.memberName;
			abiCfg.readonly = m.pure;
			m.arc4MethodConfig = abiCfg;
		}
		// else: transitively-pulled — leave arc4MethodConfig empty so
		// puya emits an internal subroutine instead of an ABI route.

		// Inject ensure_budget for force-delegated ABI methods (inner-call
		// pool is the helper's only opcode budget source).
		uint64_t budgetForMethod = 0;
		if (auto it = _ensureBudget.find(m.memberName); it != _ensureBudget.end())
			budgetForMethod = it->second;
		else if (auto dot = m.memberName.rfind('.'); dot != std::string::npos)
			if (auto it2 = _ensureBudget.find(m.memberName.substr(dot + 1));
				it2 != _ensureBudget.end())
				budgetForMethod = it2->second;
		if (budgetForMethod > 0 && m.body)
		{
			auto budgetVal = awst::makeIntegerConstant(
				std::to_string(budgetForMethod), m.sourceLocation);
			auto feeSource = awst::makeIntegerConstant("0", m.sourceLocation);
			auto ebCall = std::make_shared<awst::PuyaLibCall>();
			ebCall->sourceLocation = m.sourceLocation;
			ebCall->wtype = awst::WType::voidType();
			ebCall->func = "ensure_budget";
			ebCall->args = {
				awst::CallArg{std::string("required_budget"), std::move(budgetVal)},
				awst::CallArg{std::string("fee_source"), std::move(feeSource)},
			};
			auto block = std::dynamic_pointer_cast<awst::Block>(m.body);
			if (block)
			{
				block->body.insert(block->body.begin(),
					awst::makeExpressionStatement(std::move(ebCall), m.sourceLocation));
			}
		}

		helper->methods.push_back(std::move(m));
	}

	ContractAWST helperOut;
	helperOut.contractId = helper->id;
	helperOut.contractName = helper->name;
	for (auto const& s : helperRoots) helperOut.roots.push_back(s);
	helperOut.roots.push_back(helper);
	out.push_back(std::move(helperOut));

	// Orchestrator: replace bodies of moved subroutines with stubs; for moved
	// ContractMethods, deep-copy the primary Contract and rewrite its method
	// bodies in place. Everything else passes through.
	ContractAWST orchOut;
	orchOut.contractId = primary->id;
	orchOut.contractName = primary->name;

	// Deep-copy the Contract to rewrite moved method bodies in place.
	std::shared_ptr<awst::Contract> orchContract;
	if (!movedMethodNames.empty())
	{
		orchContract = std::make_shared<awst::Contract>(*primary);
		for (auto& m : orchContract->methods)
		{
			if (!movedMethodNames.count(m.memberName)) continue;
			// Build a synthetic Subroutine view so we can reuse buildStubBody.
			awst::Subroutine syntheticSub;
			syntheticSub.name = m.memberName;
			syntheticSub.args = m.args;
			syntheticSub.returnType = m.returnType;
			syntheticSub.sourceLocation = m.sourceLocation;
			m.body = buildStubBody(syntheticSub, helper->name);
		}
	}

	// Inject __delegate_update: without it, the orch router rejects OC=UpdateApplication
	// (`txn OnCompletion; !; assert`), blocking the lonely-chunk's install step.
	// Body is intentionally unguarded (caller-verification is a TODO).
	if (!orchContract && !movedSubNames.empty())
		orchContract = std::make_shared<awst::Contract>(*primary);
	if (orchContract)
	{
		auto loc = orchContract->sourceLocation;
		awst::ContractMethod hatch;
		hatch.cref = orchContract->id;
		hatch.memberName = "__delegate_update";
		hatch.returnType = awst::WType::voidType();
		hatch.sourceLocation = loc;
		auto block = std::make_shared<awst::Block>();
		block->sourceLocation = loc;
		// Empty body; puya emits a stub that returns 1 and lets the AVM
		// apply UpdateApplication after our handler completes.
		hatch.body = std::move(block);
		awst::ARC4ABIMethodConfig abiCfg;
		abiCfg.sourceLocation = loc;
		abiCfg.allowedCompletionTypes = {4}; // UpdateApplication
		abiCfg.create = 3;                    // Disallow
		abiCfg.name = "__delegate_update";
		hatch.arc4MethodConfig = abiCfg;
		orchContract->methods.push_back(std::move(hatch));
	}

	for (auto const& r : _roots)
	{
		if (auto s = std::dynamic_pointer_cast<awst::Subroutine>(r))
		{
			if (movedSubNames.count(s->name))
			{
				auto stub = std::make_shared<awst::Subroutine>(*s);
				stub->body = buildStubBody(*s, helper->name);
				orchOut.roots.push_back(std::move(stub));
				continue;
			}
		}
		else if (orchContract && r.get() == primary.get())
		{
			// Replace the original Contract pointer with our edited copy.
			orchOut.roots.push_back(orchContract);
			continue;
		}
		orchOut.roots.push_back(r);
	}
	out.push_back(std::move(orchOut));

	return out;
}

} // namespace puyasol::splitter
