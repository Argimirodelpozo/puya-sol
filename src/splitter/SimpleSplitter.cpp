/// @file SimpleSplitter.cpp
/// Static "extract-named-subroutines" splitter — see SimpleSplitter.h.

#include "splitter/SimpleSplitter.h"
#include "Logger.h"
#include "awst/WType.h"

#include <memory>
#include <set>
#include <vector>

namespace puyasol::splitter
{

namespace
{

constexpr int TxnTypeAppl = 6;

// puya's ABI type-name conventions for the wtypes ctf-exchange uses.
// (uint512 == biguint, byte[N] == fixed bytes, byte[] == dynamic bytes.)
//
// CRITICAL: this name has to match what puya generates for the helper's
// method signature, since the selector (sha512_256("name(args)return")[:4])
// has to dispatch correctly across contracts. WTuple/ARC4Tuple need to be
// expanded as "(t0,t1,...)", strings as "string", and ARC4 composite types
// as the original Solidity-level type so we don't generate a selector that
// targets a different overload.
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

/// Encode a single argument expression for inclusion in ApplicationArgs.
/// puya expects ABI-encoded bytes per arg.
std::shared_ptr<awst::Expression> encodeArg(
	std::shared_ptr<awst::Expression> argExpr,
	awst::SourceLocation const& loc)
{
	auto const* wt = argExpr->wtype;
	if (wt == awst::WType::accountType()
		|| (wt && wt->kind() == awst::WTypeKind::Bytes))
	{
		return awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), loc);
	}
	if (wt == awst::WType::biguintType())
	{
		return awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), loc);
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
	// ARC4 composite types (Struct/Array/Tuple/UIntN): the in-memory
	// representation IS the ARC4-encoded byte string. Reinterpret to bytes.
	if (wt && (wt->kind() == awst::WTypeKind::ARC4Struct
		|| wt->kind() == awst::WTypeKind::ARC4StaticArray
		|| wt->kind() == awst::WTypeKind::ARC4DynamicArray
		|| wt->kind() == awst::WTypeKind::ARC4Tuple
		|| wt->kind() == awst::WTypeKind::ARC4UIntN
		|| wt->kind() == awst::WTypeKind::ARC4UFixedNxM))
	{
		return awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), loc);
	}
	// Native WTuple: needs ARC4 encoding to flatten into the ABI byte form.
	if (wt && wt->kind() == awst::WTypeKind::WTuple)
	{
		auto enc = std::make_shared<awst::ARC4Encode>();
		enc->sourceLocation = loc;
		// Use a default ARC4 tuple wtype; puya derives the encoding from the
		// inner expression's wtype. The result is bytes.
		enc->wtype = awst::WType::bytesType();
		enc->value = std::move(argExpr);
		return awst::makeReinterpretCast(std::move(enc), awst::WType::bytesType(), loc);
	}
	return awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), loc);
}

/// Map a WType to its ARC4 equivalent, used to build the bytes-shape that
/// ARC4Decode consumes. Returns nullptr if the WType has no known ARC4
/// counterpart we support. WTuple recursion mints fresh ARC4Tuples, so we
/// stash them in a static owned arena that frees at process exit.
awst::WType const* mapToArc4(awst::WType const* w)
{
	if (!w) return nullptr;
	if (w == awst::WType::biguintType())
	{
		static awst::ARC4UIntN s_uint256(32);
		return &s_uint256;
	}
	if (w == awst::WType::uint64Type())
	{
		static awst::ARC4UIntN s_uint64(8);
		return &s_uint64;
	}
	if (w == awst::WType::boolType())
	{
		return awst::WType::arc4BoolType();
	}
	if (w == awst::WType::accountType())
	{
		static awst::ARC4UIntN s_uint256(32);
		return &s_uint256;
	}
	if (w == awst::WType::stringType())
	{
		// arc4.string == dynamic_array<arc4.byte>, immutable=true.
		// Match puya's `arc4_string_alias` exactly so its decode router
		// accepts the bytes shape as a Solidity `string`.
		static awst::ARC4UIntN s_arc4Byte(8, "byte");
		static awst::ARC4DynamicArray s_arc4String(&s_arc4Byte, "string", /*immutable=*/true);
		return &s_arc4String;
	}
	if (w->kind() == awst::WTypeKind::Bytes
		&& dynamic_cast<awst::BytesWType const*>(w)
		&& !dynamic_cast<awst::BytesWType const*>(w)->length())
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
	if (retType && retType->kind() == awst::WTypeKind::WTuple)
	{
		// Reinterpret the post-prefix bytes as an ARC4 tuple, then decode to
		// the native WTuple. Without ARC4Decode, puya would see a bytes value
		// where it expects a WTuple at the call site.
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

/// Walk an expression/statement tree and collect SubroutineID target IDs
/// that are referenced. Used to discover transitive deps of moved subs
/// so they can be co-located in the helper.
void collectSubroutineIds(awst::Expression const& e, std::set<std::string>& out);
void collectSubroutineIds(awst::Statement const& s, std::set<std::string>& out);

void collectSubroutineIds(awst::Expression const& e, std::set<std::string>& out)
{
	if (auto const* sce = dynamic_cast<awst::SubroutineCallExpression const*>(&e))
	{
		if (auto const* id = std::get_if<awst::SubroutineID>(&sce->target))
			out.insert(id->target);
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

/// Compute the closure of subroutines transitively called by any of `seeds`,
/// limited to functions present in `subById`. Used to bring deps along when
/// extracting a sub to a helper.
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

/// Build a helper contract that exposes the given subs as ABI methods.
/// The helper's ABI methods just call the helper-local subroutine.
std::shared_ptr<awst::Contract> buildHelperContract(
	awst::Contract const& original,
	std::vector<std::shared_ptr<awst::Subroutine>> const& subs,
	int helperIdx)
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
	int _helperIndex)
{
	std::vector<ContractAWST> out;

	std::shared_ptr<awst::Contract> primary;
	std::vector<std::shared_ptr<awst::Subroutine>> moved;
	std::set<std::string> moveSet(_moveNames.begin(), _moveNames.end());

	// Args/returns of these kinds round-trip cleanly through the inner-call
	// stub: their puya-side runtime representation IS the ARC4-encoded byte
	// string, so encode/decode is a reinterpret-cast. WTuple needs an extra
	// ARC4Encode on the way out, which encodeArg handles.
	auto isUnsupported = [](awst::WType const* t) {
		if (!t) return false;
		// Currently no kinds are unsupported as args. Returns of WTuple/
		// ARC4Struct still need work in decodeReturn — rejected separately
		// below.
		(void)t;
		return false;
	};
	// Return types we can decode cleanly. ARC4 composite types reinterpret
	// directly; WTuple decodes via mapToArc4 + ARC4Decode if every element
	// has a known ARC4 counterpart (string/bytes still bail).
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
			// Skip subs with composite return types (tuple/struct/array) —
			// the inner-call decode path doesn't handle them yet, and they're
			// rarely worth extracting anyway. Same for arg types.
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
	// Methods can reference subroutines too (e.g. _verifyECDSASignature →
	// ECDSA.recover); walk their bodies and pull in any subs the helper
	// will need.
	{
		std::set<std::string> seenIds;
		for (auto const& s : helperRoots) seenIds.insert(s->id);
		std::set<std::string> refs;
		for (auto const& m : movedMethods)
			if (m.body) collectSubroutineIds(*m.body, refs);
		std::vector<std::shared_ptr<awst::Subroutine>> seeds;
		for (auto const& id : refs)
		{
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

	// Helper contract first (so test harness can deploy it before orchestrator
	// reads its app id from a template var).
	auto helper = buildHelperContract(*primary, moved, _helperIndex);

	// Add moved ContractMethods to the helper. Re-cref to helper, mark as
	// ARC4 ABI, and append. The helper's ARC4Router will dispatch to them
	// alongside the subroutine wrappers.
	for (auto m : movedMethods)
	{
		m.cref = helper->id;
		awst::ARC4ABIMethodConfig abiCfg;
		abiCfg.sourceLocation = m.sourceLocation;
		abiCfg.allowedCompletionTypes = {0};
		abiCfg.create = 3;
		abiCfg.name = m.memberName;
		abiCfg.readonly = m.pure;
		m.arc4MethodConfig = abiCfg;
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

	// Deep-copy the primary Contract (vector of ContractMethods is a value
	// member, so this gives us a free copy of the methods table).
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
