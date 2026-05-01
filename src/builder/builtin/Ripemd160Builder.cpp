/// @file Ripemd160Builder.cpp
///
/// AWST builder for the RIPEMD-160 hash subroutine. Direct port of
/// tiny-ripemd160 (MIT, Dave Turner). All 32-bit unsigned arithmetic is
/// simulated in uint64 with explicit `& 0xFFFFFFFF` masking.
///
/// Subroutine signature:
///   __builtin_ripemd160(data: bytes) -> bytes
///
/// Compactness strategy:
///   - All static data (shifts, two index tables, fn-ids, ks per line) lives
///     in byte tables read with `getbyte`.
///   - Outer round and inner 16-step loops are runtime `while` loops, not
///     unrolled — each line is a body that processes 5×16 steps.
///   - The 5 round-functions f1..f5 dispatch through a ConditionalExpression
///     chain on the round-indexed fn-id.
///   - x[idx] (the 16 LE words per chunk) is computed on demand via
///     `readLeWord(padded, pos + idx*4)` — avoids materializing 16 locals.
///
/// Body structure (high level):
///   1. Initialize digest state h0..h4 with the canonical IV.
///   2. Pad input: append 0x80, zero-fill, append 8-byte length-in-bits LE.
///      This yields a multiple of 64 bytes.
///   3. For each 64-byte chunk:
///        a. Run the "left line" — 5×16 step round loop with the identity
///           initial index permutation, fns[1..5], constants_left.
///        b. Run the "right line" — same shape with the 5+9i initial index
///           permutation, fns[5..1], constants_right.
///        c. Combine left/right into the digest with the cross pattern and
///           perform the final 1-step rotation.
///   4. Concatenate h0..h4 (each as 4 LE bytes) into the 20-byte digest.

#include "builder/builtin/Ripemd160Builder.h"

#include <utility>
#include <vector>

namespace puyasol::builder::builtin
{

using namespace puyasol::awst;

namespace
{

std::string const SUBROUTINE_ID = "__builtin_ripemd160";

// ── Small AWST helpers ─────────────────────────────────────────────────

std::shared_ptr<Expression> u64Const(uint64_t v, SourceLocation const& loc)
{
	return makeIntegerConstant(std::to_string(v), loc, WType::uint64Type());
}

std::shared_ptr<VarExpression> u64Var(std::string const& name, SourceLocation const& loc)
{
	return makeVarExpression(name, WType::uint64Type(), loc);
}

std::shared_ptr<VarExpression> bytesVar(std::string const& name, SourceLocation const& loc)
{
	return makeVarExpression(name, WType::bytesType(), loc);
}

std::shared_ptr<Expression> u64BinOp(
	std::shared_ptr<Expression> l, UInt64BinaryOperator op, std::shared_ptr<Expression> r,
	SourceLocation const& loc)
{
	return makeUInt64BinOp(std::move(l), op, std::move(r), loc);
}

std::shared_ptr<Expression> u64BinOp(
	std::shared_ptr<Expression> l, UInt64BinaryOperator op, uint64_t r,
	SourceLocation const& loc)
{
	return makeUInt64BinOp(std::move(l), op, u64Const(r, loc), loc);
}

std::shared_ptr<Expression> mask32(std::shared_ptr<Expression> expr, SourceLocation const& loc)
{
	return u64BinOp(std::move(expr), UInt64BinaryOperator::BitAnd, 0xFFFFFFFFULL, loc);
}

std::shared_ptr<Expression> add32(
	std::shared_ptr<Expression> a, std::shared_ptr<Expression> b, SourceLocation const& loc)
{
	return mask32(u64BinOp(std::move(a), UInt64BinaryOperator::Add, std::move(b), loc), loc);
}

/// ROL32(<var name>, <var name>) — rotate uint64-as-uint32 left by `nName`.
/// Result: ((x << n) | (x >> (32-n))) & 0xFFFFFFFF.
std::shared_ptr<Expression> rol32Var(
	std::string const& xName, std::string const& nName, SourceLocation const& loc)
{
	auto lo = u64BinOp(u64Var(xName, loc), UInt64BinaryOperator::LShift,
		u64Var(nName, loc), loc);
	auto comp = u64BinOp(u64Const(32, loc), UInt64BinaryOperator::Sub, u64Var(nName, loc), loc);
	auto hi = u64BinOp(u64Var(xName, loc), UInt64BinaryOperator::RShift,
		std::move(comp), loc);
	return mask32(u64BinOp(std::move(lo), UInt64BinaryOperator::BitOr, std::move(hi), loc), loc);
}

/// ROL32 with compile-time `n` (used for the fixed `ROL(w2, 10)` step).
std::shared_ptr<Expression> rol32Const(
	std::string const& xName, uint64_t n, SourceLocation const& loc)
{
	auto lo = u64BinOp(u64Var(xName, loc), UInt64BinaryOperator::LShift,
		u64Const(n, loc), loc);
	auto hi = u64BinOp(u64Var(xName, loc), UInt64BinaryOperator::RShift,
		u64Const(32 - n, loc), loc);
	return mask32(u64BinOp(std::move(lo), UInt64BinaryOperator::BitOr, std::move(hi), loc), loc);
}

std::shared_ptr<Expression> not32(std::shared_ptr<Expression> x, SourceLocation const& loc)
{
	return u64BinOp(std::move(x), UInt64BinaryOperator::BitXor, 0xFFFFFFFFULL, loc);
}

std::shared_ptr<Statement> assignStmt(
	std::shared_ptr<Expression> target, std::shared_ptr<Expression> value,
	SourceLocation const& loc)
{
	return makeAssignmentStatement(std::move(target), std::move(value), loc);
}

std::shared_ptr<Block> makeBlock(SourceLocation const& loc)
{
	auto b = std::make_shared<Block>();
	b->sourceLocation = loc;
	return b;
}

std::shared_ptr<Expression> bytesLen(std::shared_ptr<Expression> b, SourceLocation const& loc)
{
	auto call = makeIntrinsicCall("len", WType::uint64Type(), loc);
	call->stackArgs.push_back(std::move(b));
	return call;
}

std::shared_ptr<Expression> bzeroOf(std::shared_ptr<Expression> n, SourceLocation const& loc)
{
	auto call = makeIntrinsicCall("bzero", WType::bytesType(), loc);
	call->stackArgs.push_back(std::move(n));
	return call;
}

std::shared_ptr<Expression> concatBytes(
	std::shared_ptr<Expression> a, std::shared_ptr<Expression> b, SourceLocation const& loc)
{
	auto call = makeIntrinsicCall("concat", WType::bytesType(), loc);
	call->stackArgs.push_back(std::move(a));
	call->stackArgs.push_back(std::move(b));
	return call;
}

std::shared_ptr<Expression> getByte(
	std::shared_ptr<Expression> b, std::shared_ptr<Expression> off,
	SourceLocation const& loc)
{
	auto call = makeIntrinsicCall("getbyte", WType::uint64Type(), loc);
	call->stackArgs.push_back(std::move(b));
	call->stackArgs.push_back(std::move(off));
	return call;
}

std::shared_ptr<Expression> setByte(
	std::shared_ptr<Expression> b, std::shared_ptr<Expression> off,
	std::shared_ptr<Expression> val, SourceLocation const& loc)
{
	auto call = makeIntrinsicCall("setbyte", WType::bytesType(), loc);
	call->stackArgs.push_back(std::move(b));
	call->stackArgs.push_back(std::move(off));
	call->stackArgs.push_back(std::move(val));
	return call;
}

std::shared_ptr<Expression> oneByte(uint8_t value, SourceLocation const& loc)
{
	return makeBytesConstant({value}, loc, BytesEncoding::Base16);
}

// Read 4 bytes from local var `bName` at offset (var `offName`) as a
// little-endian uint32 → uint64. Emits fresh VarExpressions every read.
std::shared_ptr<Expression> readLeWord(
	std::string const& bName, std::string const& offName,
	SourceLocation const& loc)
{
	auto byteAt = [&](uint64_t i) {
		auto off = u64BinOp(u64Var(offName, loc), UInt64BinaryOperator::Add,
			u64Const(i, loc), loc);
		return getByte(bytesVar(bName, loc), std::move(off), loc);
	};
	auto b0 = byteAt(0);
	auto b1 = u64BinOp(byteAt(1), UInt64BinaryOperator::LShift, 8, loc);
	auto b2 = u64BinOp(byteAt(2), UInt64BinaryOperator::LShift, 16, loc);
	auto b3 = u64BinOp(byteAt(3), UInt64BinaryOperator::LShift, 24, loc);
	auto or01 = u64BinOp(std::move(b0), UInt64BinaryOperator::BitOr, std::move(b1), loc);
	auto or012 = u64BinOp(std::move(or01), UInt64BinaryOperator::BitOr, std::move(b2), loc);
	return u64BinOp(std::move(or012), UInt64BinaryOperator::BitOr, std::move(b3), loc);
}

// Encode a uint32-in-uint64 var (named) as 4 LE bytes.
std::shared_ptr<Expression> wordLeBytes(
	std::string const& wName, SourceLocation const& loc)
{
	auto byte = [&](uint64_t shift) {
		return u64BinOp(
			u64BinOp(u64Var(wName, loc), UInt64BinaryOperator::RShift, shift, loc),
			UInt64BinaryOperator::BitAnd, 0xFFULL, loc);
	};
	auto buf = bzeroOf(u64Const(4, loc), loc);
	buf = setByte(std::move(buf), u64Const(0, loc), byte(0), loc);
	buf = setByte(std::move(buf), u64Const(1, loc), byte(8), loc);
	buf = setByte(std::move(buf), u64Const(2, loc), byte(16), loc);
	buf = setByte(std::move(buf), u64Const(3, loc), byte(24), loc);
	return buf;
}

// ── Static tables ──────────────────────────────────────────────────────

std::vector<uint8_t> shiftsTable()
{
	return {
		11,14,15,12, 5, 8, 7, 9,11,13,14,15, 6, 7, 9, 8,
		12,13,11,15, 6, 9, 9, 7,12,15,11,13, 7, 8, 7, 7,
		13,15,14,11, 7, 7, 6, 8,13,14,13,12, 5, 5, 6, 9,
		14,11,12,14, 8, 6, 5, 5,15,12,15,14, 9, 9, 8, 6,
		15,12,13,13, 9, 5, 8, 6,14,11,12,11, 8, 6, 5, 5
	};
}

std::vector<uint8_t> rhoTable()
{
	return { 7, 4,13, 1,10, 6,15, 3,12, 0, 9, 5, 2,14,11, 8 };
}

std::vector<uint8_t> indexLine(uint8_t start, uint8_t step)
{
	std::vector<uint8_t> v(16);
	v[0] = start & 0x0F;
	for (size_t i = 1; i < 16; ++i)
		v[i] = (uint8_t)((v[i - 1] + step) & 0x0F);
	return v;
}

// 5×16 = 80 byte index table (one row per round, with rho applied per-round).
std::vector<uint8_t> fullIndexTable(std::vector<uint8_t> initial)
{
	std::vector<uint8_t> rho = rhoTable();
	std::vector<uint8_t> all;
	all.reserve(80);
	auto cur = std::move(initial);
	for (int round = 0; round < 5; ++round)
	{
		for (uint8_t v : cur) all.push_back(v);
		std::vector<uint8_t> next(16);
		for (size_t i = 0; i < 16; ++i) next[i] = rho[cur[i]];
		cur = std::move(next);
	}
	return all;
}

// 5-byte k-id table (we look up the actual constant via a switch).
// Not strictly needed since we encode round directly.

// ── Round function dispatch via ConditionalExpression chain ────────────
// Builds: (fn==1 ? f1 : (fn==2 ? f2 : (fn==3 ? f3 : (fn==4 ? f4 : f5))))
// where each f_n is built from var names. Each branch references w1/w2/w3
// freshly so the AWST is a tree.
std::shared_ptr<Expression> buildFnDispatch(
	std::string const& fnName,
	std::string const& bName, std::string const& cName, std::string const& dName,
	SourceLocation const& loc)
{
	using O = UInt64BinaryOperator;
	auto b = [&] { return u64Var(bName, loc); };
	auto c = [&] { return u64Var(cName, loc); };
	auto d = [&] { return u64Var(dName, loc); };
	auto Xor = [&](auto x, auto y) { return u64BinOp(std::move(x), O::BitXor, std::move(y), loc); };
	auto And = [&](auto x, auto y) { return u64BinOp(std::move(x), O::BitAnd, std::move(y), loc); };
	auto Or  = [&](auto x, auto y) { return u64BinOp(std::move(x), O::BitOr,  std::move(y), loc); };

	auto eq = [&](uint64_t v) {
		return makeNumericCompare(u64Var(fnName, loc), NumericComparison::Eq, u64Const(v, loc), loc);
	};
	auto cond = [&](std::shared_ptr<Expression> c0,
					std::shared_ptr<Expression> t,
					std::shared_ptr<Expression> e) {
		auto ce = std::make_shared<ConditionalExpression>();
		ce->sourceLocation = loc;
		ce->wtype = WType::uint64Type();
		ce->condition = std::move(c0);
		ce->trueExpr = std::move(t);
		ce->falseExpr = std::move(e);
		return ce;
	};

	// f1: b ^ c ^ d
	auto f1 = Xor(Xor(b(), c()), d());
	// f2: (b & c) | (~b & d)
	auto f2 = mask32(Or(And(b(), c()), And(not32(b(), loc), d())), loc);
	// f3: (b | ~c) ^ d
	auto f3 = Xor(Or(b(), not32(c(), loc)), d());
	// f4: (b & d) | (c & ~d)
	auto f4 = mask32(Or(And(b(), d()), And(c(), not32(d(), loc))), loc);
	// f5: b ^ (c | ~d)
	auto f5 = Xor(b(), Or(c(), not32(d(), loc)));

	// Nested ternaries
	auto inner4 = cond(eq(4), std::move(f4), std::move(f5));
	auto inner3 = cond(eq(3), std::move(f3), std::move(inner4));
	auto inner2 = cond(eq(2), std::move(f2), std::move(inner3));
	return cond(eq(1), std::move(f1), std::move(inner2));
}

// ── One-line body emitter (left or right) ─────────────────────────────
//
// Initializes Lw0..Lw4 (or Rw0..Rw4) from h0..h4, then runs a 2-level
// runtime loop:
//   for round in [0..5):
//     for i in [0..16):
//       idx   = getbyte(idxTable, round*16 + i)
//       shift = getbyte(shTable, round*16 + i)
//       fn    = getbyte(fnTable, round)         // 1..5
//       k     = lookupK(round)                  // ConditionalExpression chain
//       xi    = readLeWord(padded, pos + idx*4)
//       sum   = w0 + f_fn(w1,w2,w3) + xi + k
//       sum   = ROL(sum, shift) + w4
//       // rotate
//       tmp = sum
//       w0  = w4
//       w4  = w3
//       w3  = ROL(w2, 10)
//       w2  = w1
//       w1  = tmp
void emitLine(
	Block& body,
	std::vector<uint8_t> const& indexTbl,    // 80-byte
	std::vector<uint8_t> const& fnTbl,       // 5-byte
	std::vector<uint64_t> const& ksTbl,      // 5 entries
	std::string const& wPfx,
	SourceLocation const& loc)
{
	// Initialize w0..w4 from digest h0..h4.
	for (int i = 0; i < 5; ++i)
		body.body.push_back(assignStmt(
			u64Var(wPfx + std::to_string(i), loc),
			u64Var("h" + std::to_string(i), loc), loc));

	// Bake constants tables as bytes literals — we let puya hoist these
	// to bytecblock constants in the TEAL output.
	std::string const idxTblName = wPfx + "_idx";
	std::string const fnTblName = wPfx + "_fn";
	body.body.push_back(assignStmt(bytesVar(idxTblName, loc),
		makeBytesConstant(indexTbl, loc, BytesEncoding::Base16), loc));
	body.body.push_back(assignStmt(bytesVar(fnTblName, loc),
		makeBytesConstant(fnTbl, loc, BytesEncoding::Base16), loc));
	// Emit a shared shifts table (unique per call but symbolically the same
	// constant — puya dedupes across calls).
	std::string const shTblName = wPfx + "_sh";
	body.body.push_back(assignStmt(bytesVar(shTblName, loc),
		makeBytesConstant(shiftsTable(), loc, BytesEncoding::Base16), loc));

	// Round loop: round = 0; while round < 5: ...
	body.body.push_back(assignStmt(u64Var("round", loc), u64Const(0, loc), loc));

	auto roundLoop = std::make_shared<WhileLoop>();
	roundLoop->sourceLocation = loc;
	roundLoop->condition = makeNumericCompare(
		u64Var("round", loc), NumericComparison::Lt, u64Const(5, loc), loc);
	auto roundBody = makeBlock(loc);

	// fn = getbyte(fnTbl, round)
	roundBody->body.push_back(assignStmt(u64Var("fn", loc),
		getByte(bytesVar(fnTblName, loc), u64Var("round", loc), loc), loc));

	// k = lookupK(round) — a 4-way ternary chain on the 5 ks.
	{
		auto eq = [&](uint64_t v) {
			return makeNumericCompare(u64Var("round", loc), NumericComparison::Eq, u64Const(v, loc), loc);
		};
		auto cond = [&](std::shared_ptr<Expression> c0,
						std::shared_ptr<Expression> t,
						std::shared_ptr<Expression> e) {
			auto ce = std::make_shared<ConditionalExpression>();
			ce->sourceLocation = loc;
			ce->wtype = WType::uint64Type();
			ce->condition = std::move(c0);
			ce->trueExpr = std::move(t);
			ce->falseExpr = std::move(e);
			return ce;
		};
		auto k = cond(eq(0), u64Const(ksTbl[0], loc),
			cond(eq(1), u64Const(ksTbl[1], loc),
				cond(eq(2), u64Const(ksTbl[2], loc),
					cond(eq(3), u64Const(ksTbl[3], loc), u64Const(ksTbl[4], loc)))));
		roundBody->body.push_back(assignStmt(u64Var("k", loc), std::move(k), loc));
	}

	// roundBase = round * 16
	roundBody->body.push_back(assignStmt(u64Var("roundBase", loc),
		u64BinOp(u64Var("round", loc), UInt64BinaryOperator::Mult, 16, loc), loc));

	// Inner loop: i = 0; while i < 16: ...
	roundBody->body.push_back(assignStmt(u64Var("i", loc), u64Const(0, loc), loc));

	auto innerLoop = std::make_shared<WhileLoop>();
	innerLoop->sourceLocation = loc;
	innerLoop->condition = makeNumericCompare(
		u64Var("i", loc), NumericComparison::Lt, u64Const(16, loc), loc);
	auto innerBody = makeBlock(loc);

	// idx   = getbyte(idxTbl, roundBase + i)
	// shift = getbyte(shTbl,  roundBase + idx)   // shift indexed by index[i]
	auto roundBasePlusI = [&] {
		return u64BinOp(u64Var("roundBase", loc), UInt64BinaryOperator::Add,
			u64Var("i", loc), loc);
	};
	innerBody->body.push_back(assignStmt(u64Var("idx", loc),
		getByte(bytesVar(idxTblName, loc), roundBasePlusI(), loc), loc));
	innerBody->body.push_back(assignStmt(u64Var("shift", loc),
		getByte(bytesVar(shTblName, loc),
			u64BinOp(u64Var("roundBase", loc), UInt64BinaryOperator::Add,
				u64Var("idx", loc), loc), loc), loc));

	// xOff = pos + idx * 4   (padded byte offset of word x[idx])
	innerBody->body.push_back(assignStmt(u64Var("xOff", loc),
		u64BinOp(u64Var("pos", loc), UInt64BinaryOperator::Add,
			u64BinOp(u64Var("idx", loc), UInt64BinaryOperator::Mult, 4, loc), loc), loc));
	innerBody->body.push_back(assignStmt(u64Var("xi", loc),
		readLeWord("padded", "xOff", loc), loc));

	// fnVal = dispatch(fn, w1, w2, w3)
	innerBody->body.push_back(assignStmt(u64Var("fnVal", loc),
		buildFnDispatch("fn", wPfx + "1", wPfx + "2", wPfx + "3", loc), loc));

	// sum = w0 + fnVal + xi + k
	auto sum = add32(
		add32(
			add32(u64Var(wPfx + "0", loc), u64Var("fnVal", loc), loc),
			u64Var("xi", loc), loc),
		u64Var("k", loc), loc);
	innerBody->body.push_back(assignStmt(u64Var("sum", loc), std::move(sum), loc));

	// sum = ROL(sum, shift) + w4
	auto rolSum = rol32Var("sum", "shift", loc);
	innerBody->body.push_back(assignStmt(u64Var("sum", loc),
		add32(std::move(rolSum), u64Var(wPfx + "4", loc), loc), loc));

	// Word rotation: tmp = sum; w0 = w4; w4 = w3; w3 = ROL(w2, 10);
	// w2 = w1; w1 = tmp
	innerBody->body.push_back(assignStmt(u64Var("tmp", loc), u64Var("sum", loc), loc));
	innerBody->body.push_back(assignStmt(u64Var(wPfx + "0", loc),
		u64Var(wPfx + "4", loc), loc));
	innerBody->body.push_back(assignStmt(u64Var(wPfx + "4", loc),
		u64Var(wPfx + "3", loc), loc));
	innerBody->body.push_back(assignStmt(u64Var(wPfx + "3", loc),
		rol32Const(wPfx + "2", 10, loc), loc));
	innerBody->body.push_back(assignStmt(u64Var(wPfx + "2", loc),
		u64Var(wPfx + "1", loc), loc));
	innerBody->body.push_back(assignStmt(u64Var(wPfx + "1", loc),
		u64Var("tmp", loc), loc));

	// i = i + 1
	innerBody->body.push_back(assignStmt(u64Var("i", loc),
		u64BinOp(u64Var("i", loc), UInt64BinaryOperator::Add, 1, loc), loc));
	innerLoop->loopBody = std::move(innerBody);
	roundBody->body.push_back(innerLoop);

	// round = round + 1
	roundBody->body.push_back(assignStmt(u64Var("round", loc),
		u64BinOp(u64Var("round", loc), UInt64BinaryOperator::Add, 1, loc), loc));
	roundLoop->loopBody = std::move(roundBody);
	body.body.push_back(roundLoop);
}

void emitProcessChunk(Block& body, SourceLocation const& loc)
{
	// Left line.
	auto leftIdx = fullIndexTable(indexLine(0, 1));
	std::vector<uint8_t> fnLeftTbl = { 1, 2, 3, 4, 5 };
	std::vector<uint64_t> ksLeftTbl =
		{ 0x00000000ULL, 0x5a827999ULL, 0x6ed9eba1ULL, 0x8f1bbcdcULL, 0xa953fd4eULL };
	emitLine(body, leftIdx, fnLeftTbl, ksLeftTbl, "L", loc);

	// Right line.
	auto rightIdx = fullIndexTable(indexLine(5, 9));
	std::vector<uint8_t> fnRightTbl = { 5, 4, 3, 2, 1 };
	std::vector<uint64_t> ksRightTbl =
		{ 0x50a28be6ULL, 0x5c4dd124ULL, 0x6d703ef3ULL, 0x7a6d76e9ULL, 0x00000000ULL };
	emitLine(body, rightIdx, fnRightTbl, ksRightTbl, "R", loc);

	// Combine + final rotation:
	//   t  = h0 + L1 + R2
	//   h0 = h1 + L2 + R3
	//   h1 = h2 + L3 + R4
	//   h2 = h3 + L4 + R0
	//   h3 = h4 + L0 + R1
	//   h4 = t
	auto combine = [&](std::string const& dst, std::string const& hSrc, int li, int ri) {
		auto val = add32(
			add32(u64Var(hSrc, loc),
				u64Var("L" + std::to_string(li), loc), loc),
			u64Var("R" + std::to_string(ri), loc), loc);
		body.body.push_back(assignStmt(u64Var(dst, loc), std::move(val), loc));
	};
	body.body.push_back(assignStmt(
		u64Var("htmp", loc),
		add32(add32(u64Var("h0", loc), u64Var("L1", loc), loc),
			u64Var("R2", loc), loc), loc));
	combine("h0", "h1", 2, 3);
	combine("h1", "h2", 3, 4);
	combine("h2", "h3", 4, 0);
	combine("h3", "h4", 0, 1);
	body.body.push_back(assignStmt(u64Var("h4", loc), u64Var("htmp", loc), loc));
}

} // anonymous namespace

std::string const& ripemd160SubroutineId() { return SUBROUTINE_ID; }
std::string const& ripemd160SubroutineName() { return SUBROUTINE_ID; }

std::shared_ptr<Subroutine> buildRipemd160Subroutine(SourceLocation loc)
{
	auto sub = std::make_shared<Subroutine>();
	sub->sourceLocation = loc;
	sub->id = SUBROUTINE_ID;
	sub->name = SUBROUTINE_ID;
	sub->returnType = WType::bytesType();
	sub->pure = true;
	sub->inlineOpt = false;

	{
		SubroutineArgument arg;
		arg.name = "data";
		arg.wtype = WType::bytesType();
		arg.sourceLocation = loc;
		sub->args.push_back(std::move(arg));
	}

	auto body = makeBlock(loc);

	// Initialize digest state h0..h4 with the canonical IV.
	body->body.push_back(assignStmt(u64Var("h0", loc), u64Const(0x67452301ULL, loc), loc));
	body->body.push_back(assignStmt(u64Var("h1", loc), u64Const(0xefcdab89ULL, loc), loc));
	body->body.push_back(assignStmt(u64Var("h2", loc), u64Const(0x98badcfeULL, loc), loc));
	body->body.push_back(assignStmt(u64Var("h3", loc), u64Const(0x10325476ULL, loc), loc));
	body->body.push_back(assignStmt(u64Var("h4", loc), u64Const(0xc3d2e1f0ULL, loc), loc));

	// dataLen = len(data); bits = dataLen * 8
	body->body.push_back(assignStmt(u64Var("dataLen", loc),
		bytesLen(bytesVar("data", loc), loc), loc));
	body->body.push_back(assignStmt(u64Var("bits", loc),
		u64BinOp(u64Var("dataLen", loc), UInt64BinaryOperator::Mult, 8, loc), loc));

	// padLen = ((dataLen + 72) >> 6) << 6.
	body->body.push_back(assignStmt(u64Var("padLen", loc),
		u64BinOp(
			u64BinOp(
				u64BinOp(u64Var("dataLen", loc), UInt64BinaryOperator::Add, 72, loc),
				UInt64BinaryOperator::RShift, 6, loc),
			UInt64BinaryOperator::LShift, 6, loc), loc));

	// padded = data ++ 0x80 ++ bzero(padLen - dataLen - 9)
	auto zerosLen = u64BinOp(
		u64BinOp(u64Var("padLen", loc), UInt64BinaryOperator::Sub, u64Var("dataLen", loc), loc),
		UInt64BinaryOperator::Sub, u64Const(9, loc), loc);
	auto firstPart = concatBytes(
		concatBytes(bytesVar("data", loc), oneByte(0x80, loc), loc),
		bzeroOf(std::move(zerosLen), loc), loc);
	body->body.push_back(assignStmt(bytesVar("padded", loc), std::move(firstPart), loc));

	// 8-byte LE length of bits at the end of the last chunk.
	auto bitsByte = [&](uint64_t shift) {
		return u64BinOp(
			u64BinOp(u64Var("bits", loc), UInt64BinaryOperator::RShift, shift, loc),
			UInt64BinaryOperator::BitAnd, 0xFFULL, loc);
	};
	auto lenLeBuf = bzeroOf(u64Const(8, loc), loc);
	for (int i = 0; i < 8; ++i)
		lenLeBuf = setByte(std::move(lenLeBuf), u64Const(i, loc), bitsByte(i * 8), loc);
	body->body.push_back(assignStmt(bytesVar("padded", loc),
		concatBytes(bytesVar("padded", loc), std::move(lenLeBuf), loc), loc));

	// Chunk loop: for pos = 0; pos < padLen; pos += 64
	body->body.push_back(assignStmt(u64Var("pos", loc), u64Const(0, loc), loc));
	auto chunkLoop = std::make_shared<WhileLoop>();
	chunkLoop->sourceLocation = loc;
	chunkLoop->condition = makeNumericCompare(
		u64Var("pos", loc), NumericComparison::Lt, u64Var("padLen", loc), loc);
	auto chunkBody = makeBlock(loc);
	emitProcessChunk(*chunkBody, loc);
	chunkBody->body.push_back(assignStmt(u64Var("pos", loc),
		u64BinOp(u64Var("pos", loc), UInt64BinaryOperator::Add, 64, loc), loc));
	chunkLoop->loopBody = std::move(chunkBody);
	body->body.push_back(chunkLoop);

	// Final digest as 20 LE bytes.
	auto digest = concatBytes(
		concatBytes(
			concatBytes(
				concatBytes(
					wordLeBytes("h0", loc),
					wordLeBytes("h1", loc), loc),
				wordLeBytes("h2", loc), loc),
			wordLeBytes("h3", loc), loc),
		wordLeBytes("h4", loc), loc);

	body->body.push_back(makeReturnStatement(std::move(digest), loc));

	sub->body = std::move(body);
	return sub;
}

} // namespace puyasol::builder::builtin
