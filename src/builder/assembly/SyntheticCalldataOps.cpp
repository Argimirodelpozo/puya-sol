/// @file SyntheticCalldataOps.cpp
/// Synthetic EVM-ABI calldata blob: when Yul accesses calldata at a non-constant
/// offset, stand up `__cd_blob` at the assembly-block entry so dynamic calldataload
/// becomes `extract3(__cd_blob, off, 32)`.
///
/// Layout + value widening are driven by the DECLARED solc types when available
/// (possible_solc item 2): head sizes from `Type::calldataHeadSize()` (statics
/// inline their full encoded size in the head), signed sub-word params
/// sign-extend to the 32-byte word, static aggregates emit one word per leaf,
/// and sub-word-element dynamic arrays re-encode per element at runtime.
/// The blob is deliberately EVM-32-byte-word-shaped — ARC4 packing is the VALUE
/// transport; this is the offset-faithful view Yul arithmetic needs.

#include "builder/assembly/AssemblyBuilder.h"
#include "awst/NameGen.h"
#include "builder/abi/AbiEncoderBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

bool AssemblyBuilder::detectDynamicCalldataAccess(solidity::yul::Block const& _block)
{
	bool found = false;
	std::function<void(solidity::yul::Expression const&)> scanExpr;
	std::function<void(std::vector<solidity::yul::Statement> const&)> scanStmts;

	auto isCalldataOp = [](std::string const& n) {
		return n == "calldataload" || n == "calldatacopy" || n == "calldatasize";
	};

	scanExpr = [&](solidity::yul::Expression const& _expr) {
		if (found) return;
		if (auto const* id = std::get_if<solidity::yul::Identifier>(&_expr))
		{
			// A dynamic calldata param's `.offset`/`.length` is read at runtime from __cd_blob, so it
			// needs the blob stood up even when there is no calldataload/copy/size in the block.
			// Resolve to the canonical AWST name FIRST (outer locals are mangled, e.g. t__20 —
			// the pointer-name sets are keyed by the mangled form).
			std::string n = resolveVarRef(*id);
			// Bare STATIC calldata pointer (`s := s2` RHS, `s := t`): reads __cd_off_<n>,
			// which needs the blob + seeds stood up.
			if (m_calldataStaticPtrNames.count(n))
				found = true;
			auto dot = n.rfind('.');
			if (dot != std::string::npos)
			{
				std::string suffix = n.substr(dot + 1);
				if (suffix == "offset" || suffix == "length")
				{
					std::string base = n.substr(0, dot);
					auto it = m_locals.find(base);
					if ((it != m_locals.end() && isDynamicCalldataType(it->second))
						|| m_calldataPointerNames.count(base))
						found = true;
				}
			}
			return;
		}
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_expr))
		{
			std::string n = getFunctionName(call->functionName);
			if (isCalldataOp(n))
			{
				// calldataload: non-const off → dynamic.
				// calldatacopy: non-const src or len → dynamic.
				// calldatasize: always runtime → dynamic (blob provides len(__cd_blob)).
				if (n == "calldatasize")
					found = true;
				else if (n == "calldataload" && call->arguments.size() == 1)
				{
					if (!resolveConstantYulValue(call->arguments[0]))
						found = true;
				}
				else if (n == "calldatacopy" && call->arguments.size() == 3)
				{
					// ANY calldatacopy needs the blob to source calldata bytes —
					// even fully CONSTANT offsets (the handler is a silent no-op
					// without the blob; a constant-offset copy in a function with
					// no other dynamic-calldata trigger was dropped, fuzz_mem).
					found = true;
				}
			}
			for (auto const& a: call->arguments)
				scanExpr(a);
		}
	};
	scanStmts = [&](std::vector<solidity::yul::Statement> const& stmts) {
		for (auto const& s: stmts)
		{
			if (found) return;
			if (auto const* fd = std::get_if<solidity::yul::FunctionDefinition>(&s))
				scanStmts(fd->body.statements);
			else if (auto const* blk = std::get_if<solidity::yul::Block>(&s))
				scanStmts(blk->statements);
			else if (auto const* iff = std::get_if<solidity::yul::If>(&s))
			{
				scanExpr(*iff->condition);
				scanStmts(iff->body.statements);
			}
			else if (auto const* sw = std::get_if<solidity::yul::Switch>(&s))
			{
				scanExpr(*sw->expression);
				for (auto const& c: sw->cases)
					scanStmts(c.body.statements);
			}
			else if (auto const* fl = std::get_if<solidity::yul::ForLoop>(&s))
			{
				scanStmts(fl->pre.statements);
				scanExpr(*fl->condition);
				scanStmts(fl->post.statements);
				scanStmts(fl->body.statements);
			}
			else if (auto const* es = std::get_if<solidity::yul::ExpressionStatement>(&s))
				scanExpr(es->expression);
			else if (auto const* assign = std::get_if<solidity::yul::Assignment>(&s))
			{
				// A pointer WRITE (`x.offset := V` / `x.length := L`) also needs the
				// blob + seeded pointer locals stood up: without this, a write-only
				// block skipped the synthetic-calldata path entirely, so the write
				// landed in a dead generic local AND the (indent-bug) seeds read a
				// never-built __cd_blob — the "load 0 type error" of 2026-07-03.
				for (auto const& tgt: assign->variableNames)
				{
					std::string n = resolveVarRef(tgt);
					if (m_calldataStaticPtrNames.count(n))
						found = true;
					auto dot = n.rfind('.');
					if (dot != std::string::npos)
					{
						std::string suffix = n.substr(dot + 1);
						if (suffix == "offset" || suffix == "length")
						{
							std::string base = n.substr(0, dot);
							auto it = m_locals.find(base);
							if ((it != m_locals.end() && isDynamicCalldataType(it->second))
								|| m_calldataPointerNames.count(base))
								found = true;
						}
					}
				}
				scanExpr(*assign->value);
			}
			else if (auto const* var = std::get_if<solidity::yul::VariableDeclaration>(&s))
				if (var->value)
					scanExpr(*var->value);
		}
	};
	scanStmts(_block.statements);
	return found;
}

namespace
{

// Encode uint64 as 32-byte big-endian: concat(bzero(24), itob(val)).
std::shared_ptr<awst::Expression> pad32BE(
	std::shared_ptr<awst::Expression> _u64Val, awst::SourceLocation const& _loc)
{
	return awst::makeLeftPad(awst::makeItob(std::move(_u64Val), _loc), 24, _loc);
}

// Pad to a 32-byte multiple.
std::shared_ptr<awst::Expression> padTo32Multiple(
	std::shared_ptr<awst::Expression> _bytes, awst::SourceLocation const& _loc)
{
	return awst::makeRightPadTo32Multiple(std::move(_bytes), _loc);
}



// Flatten a STATIC solc type to its scalar leaves in EVM head order.
void flattenSolLeaves(
	solidity::frontend::Type const* _t,
	std::vector<solidity::frontend::Type const*>& _out)
{
	using namespace solidity::frontend;
	if (auto const* at = dynamic_cast<ArrayType const*>(_t))
	{
		if (!at->isDynamicallySized())
		{
			auto n = at->length().convert_to<size_t>();
			for (size_t i = 0; i < n; ++i)
				flattenSolLeaves(at->baseType(), _out);
			return;
		}
	}
	if (auto const* st = dynamic_cast<StructType const*>(_t))
	{
		for (auto const& m: st->structDefinition().members())
			flattenSolLeaves(m->type(), _out);
		return;
	}
	_out.push_back(_t);
}

} // anonymous

// True when the declared solc type is trustworthy for EVM-ABI layout math:
// value types, and reference types actually located in calldata. Storage-ref
// params (V4 handle-model) travel as box keys — their solc types would hit
// calldataEncodedSize solAsserts.
bool AssemblyBuilder::solTypeUsable(solidity::frontend::Type const* _t)
{
	using namespace solidity::frontend;
	if (!_t) return false;
	if (_t->isValueType()) return true;
	if (auto const* rt = dynamic_cast<ReferenceType const*>(_t))
		return rt->location() == DataLocation::CallData;
	return false;
}

// One EVM-ABI 32-byte word for a scalar leaf value. Signedness comes from the
// solc type (the WType erases it): signed sub-word sign-extends; bytesN
// left-aligns; everything else zero-pads left.
std::shared_ptr<awst::Expression> AssemblyBuilder::evmCalldataWord(
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::Type const* _solLeaf,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto const* wt = _value->wtype;
	auto u64c = [&](uint64_t v) {
		return awst::makeIntegerConstant(v, _loc, awst::WType::uint64Type());
	};
	auto const* intT = dynamic_cast<IntegerType const*>(_solLeaf);
	if (intT && intT->isSigned() && wt == awst::WType::uint64Type())
	{
		// The uint64 carrier may hold the narrow value ZERO-extended (the
		// signed-shadow model extends on Yul reads, which the blob bypasses):
		// slice the DECLARED-width low bytes, then sign-extend from there.
		unsigned nbytes = intT->numBits() / 8;
		auto full = awst::makeItob(std::move(_value), _loc);
		std::shared_ptr<awst::Expression> low = nbytes >= 8
			? std::move(full)
			: std::static_pointer_cast<awst::Expression>(awst::makeExtract3(
				std::move(full), u64c(8 - nbytes), u64c(nbytes), _loc));
		return eb::AbiEncoderBuilder::signExtendBytesTo32(std::move(low), _loc);
	}
	if (wt == awst::WType::uint64Type())
		return pad32BE(std::move(_value), _loc);
	if (wt == awst::WType::biguintType())
		// Canonical 256-bit TC for signed biguints is already sign-extended;
		// unsigned shorter values left-pad via the OR-with-zeros trick.
		return awst::makeBytesOr(
			awst::makeAsBytes(std::move(_value), _loc),
			awst::makeBzero(32, _loc), _loc);
	if (wt == awst::WType::boolType())
	{
		auto castU64 = awst::makeAsUInt64(std::move(_value), _loc);
		auto byteByVal = awst::makeItob(std::move(castU64), _loc);
		auto lastByte = awst::makeExtract3(std::move(byteByVal), u64c(7), u64c(1), _loc);
		return awst::makeConcat(awst::makeBzero(31, _loc), std::move(lastByte), _loc);
	}
	if (wt == awst::WType::accountType())
		return awst::makeAsBytes(std::move(_value), _loc);
	if (dynamic_cast<FixedBytesType const*>(_solLeaf))
		// bytesN is LEFT-aligned in its word.
		return padTo32Multiple(awst::makeAsBytes(std::move(_value), _loc), _loc);
	if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(wt))
	{
		// ARC4-typed leaf (aggregate element): decode-free left-pad; signed
		// leaves sign-extend from their ARC4 width.
		auto bytes = awst::makeAsBytes(std::move(_value), _loc);
		if (intT && intT->isSigned())
			return eb::AbiEncoderBuilder::signExtendBytesTo32(std::move(bytes), _loc);
		(void) uintN;
		return awst::makeBytesOr(std::move(bytes), awst::makeBzero(32, _loc), _loc);
	}
	return awst::makeAsBytes(std::move(_value), _loc); // best-effort
}

bool AssemblyBuilder::leafNeedsEvmWord(solidity::frontend::Type const* _solLeaf)
{
	using namespace solidity::frontend;
	if (auto const* it = dynamic_cast<IntegerType const*>(_solLeaf))
		return it->isSigned();
	return dynamic_cast<FixedBytesType const*>(_solLeaf) != nullptr;
}

solidity::frontend::Type const* AssemblyBuilder::calldataSolLeaf(
	std::string const& _name, int _i)
{
	auto const* solT = calldataSolType(_name);
	if (!solTypeUsable(solT))
		return nullptr;
	std::vector<solidity::frontend::Type const*> leaves;
	flattenSolLeaves(solT, leaves);
	if (_i >= 0 && static_cast<size_t>(_i) < leaves.size())
		return leaves[_i];
	return nullptr;
}

uint64_t AssemblyBuilder::calldataHeadSizeOf(
	std::string const& _name, awst::WType const* _type)
{
	if (auto const* solT = calldataSolType(_name); solTypeUsable(solT))
		return solT->calldataHeadSize();
	if (isDynamicCalldataType(_type))
		return 32;
	return static_cast<uint64_t>(computeFlatElementCount(_type)) * 32;
}

namespace
{

// Element/field wtype of an array/struct wtype (native ReferenceArray or ARC4).
awst::WType const* arrayElementWtype(awst::WType const* _w)
{
	if (auto const* ra = dynamic_cast<awst::ReferenceArray const*>(_w))
		return ra->elementType();
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_w))
		return sa->elementType();
	if (auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(_w))
		return da->elementType();
	return _w;
}

} // anonymous

void AssemblyBuilder::emitEvmHeadWords(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _wtype,
	solidity::frontend::Type const* _solType,
	std::vector<std::shared_ptr<awst::Expression>>& _words,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	// Navigate the SOLC structure — ONE EVM word per scalar leaf, EVM head
	// order. Decoupled from computeFlatElementCount / accessFlatElement, whose
	// ARC4-flat indexing is byte-granular for bytesN (a bytes4 element counted
	// as 4 leaves) and ARC4-decodes signed elements to biguint (dropping the
	// sign) — the two static-array calldata-layout bugs the fuzzer found.
	if (auto const* at = dynamic_cast<ArrayType const*>(_solType);
		at && !at->isDynamicallySized())
	{
		auto n = at->length().convert_to<size_t>();
		auto const* elemW = arrayElementWtype(_wtype);
		for (size_t i = 0; i < n; ++i)
		{
			auto elem = awst::makeIndexExpression(
				_value, awst::makeIntegerConstant(i, _loc), elemW, _loc);
			emitEvmHeadWords(std::move(elem), elemW, at->baseType(), _words, _loc);
		}
		return;
	}
	if (auto const* st = dynamic_cast<StructType const*>(_solType))
	{
		auto const* arc4St = dynamic_cast<awst::ARC4Struct const*>(_wtype);
		auto const& members = st->structDefinition().members();
		for (size_t i = 0; i < members.size(); ++i)
		{
			awst::WType const* fieldW = arc4St && i < arc4St->fields().size()
				? arc4St->fields()[i].second : nullptr;
			auto field = awst::makeFieldExpression(
				_value, members[i]->name(), fieldW, _loc);
			emitEvmHeadWords(std::move(field), fieldW, members[i]->type(), _words, _loc);
		}
		return;
	}
	// Scalar leaf: one EVM word (evmCalldataWord sign-extends signed ints,
	// left-aligns bytesN, per the solc leaf type).
	_words.push_back(evmCalldataWord(std::move(_value), _solType, _loc));
}

std::pair<std::shared_ptr<awst::Expression>, solidity::frontend::Type const*>
AssemblyBuilder::accessEvmLeaf(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _wtype,
	solidity::frontend::Type const* _solType,
	int _wordIndex,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	if (auto const* at = dynamic_cast<ArrayType const*>(_solType);
		at && !at->isDynamicallySized())
	{
		int perElem = static_cast<int>(at->baseType()->calldataHeadSize() / 32);
		if (perElem < 1) perElem = 1;
		int elemIdx = _wordIndex / perElem;
		int inner = _wordIndex % perElem;
		auto const* elemW = arrayElementWtype(_wtype);
		auto elem = awst::makeIndexExpression(
			std::move(_value), awst::makeIntegerConstant(elemIdx, _loc), elemW, _loc);
		return accessEvmLeaf(std::move(elem), elemW, at->baseType(), inner, _loc);
	}
	if (auto const* st = dynamic_cast<StructType const*>(_solType))
	{
		auto const* arc4St = dynamic_cast<awst::ARC4Struct const*>(_wtype);
		auto const& members = st->structDefinition().members();
		for (size_t i = 0; i < members.size(); ++i)
		{
			int mw = static_cast<int>(members[i]->type()->calldataHeadSize() / 32);
			if (mw < 1) mw = 1;
			if (_wordIndex < mw)
			{
				awst::WType const* fieldW = arc4St && i < arc4St->fields().size()
					? arc4St->fields()[i].second : nullptr;
				auto field = awst::makeFieldExpression(
					std::move(_value), members[i]->name(), fieldW, _loc);
				return accessEvmLeaf(std::move(field), fieldW, members[i]->type(), _wordIndex, _loc);
			}
			_wordIndex -= mw;
		}
	}
	return {std::move(_value), _solType};
}

std::shared_ptr<awst::Expression> AssemblyBuilder::evmStaticHeadBytes(
	std::string const& _name, awst::WType const* _type,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto paramVar = awst::makeVarExpression(_name, _type, _loc);
	auto const* solT = calldataSolType(_name);
	if (!solTypeUsable(solT))
		solT = nullptr;

	bool solAggregate = solT
		&& (dynamic_cast<ArrayType const*>(solT) || dynamic_cast<StructType const*>(solT));
	if (solAggregate)
	{
		std::vector<std::shared_ptr<awst::Expression>> words;
		emitEvmHeadWords(paramVar, _type, solT, words, _loc);
		std::shared_ptr<awst::Expression> acc;
		for (auto& w: words)
			acc = acc ? awst::makeConcat(std::move(acc), std::move(w), _loc)
					  : std::move(w);
		if (acc)
			return acc;
	}
	return evmCalldataWord(std::move(paramVar), solT, _loc);
}

bool AssemblyBuilder::isDynamicCalldataType(awst::WType const* _type) const
{
	if (!_type) return false;
	if (_type == awst::WType::bytesType()) return true;
	if (_type == awst::WType::stringType()) return true;
	if (_type->kind() == awst::WTypeKind::ARC4DynamicArray) return true;
	if (_type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(_type);
		return refArr && !refArr->arraySize().has_value();
	}
	return false;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::calldataDynOffset(
	uint64_t _headPos, awst::SourceLocation const& _loc)
{
	auto u64 = [&](uint64_t v) { return awst::makeIntegerConstant(v, _loc, awst::WType::uint64Type()); };
	// headWord = btoi(extract3(__cd_blob, headPos+24, 8))  — low 8 bytes of the 32-byte head pointer.
	auto headWord = awst::makeBtoi(awst::makeExtract3(
		awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), _loc),
		u64(_headPos + 24), u64(8), _loc), _loc);
	// .offset = headWord + 36  (4-byte selector + 32-byte length word).
	auto offU64 = awst::makeUInt64BinOp(std::move(headWord), awst::UInt64BinaryOperator::Add, u64(36), _loc);
	return awst::makeAsBiguint(awst::makeItob(std::move(offU64), _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::calldataDynLength(
	uint64_t _headPos, awst::SourceLocation const& _loc)
{
	auto u64 = [&](uint64_t v) { return awst::makeIntegerConstant(v, _loc, awst::WType::uint64Type()); };
	auto headWord = awst::makeBtoi(awst::makeExtract3(
		awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), _loc),
		u64(_headPos + 24), u64(8), _loc), _loc);
	// length word sits at byte (4 + headWord); its low 8 bytes at (4 + headWord + 24) = headWord + 28.
	auto lenPos = awst::makeUInt64BinOp(std::move(headWord), awst::UInt64BinaryOperator::Add, u64(28), _loc);
	auto lenU64 = awst::makeBtoi(awst::makeExtract3(
		awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), _loc),
		std::move(lenPos), u64(8), _loc), _loc);
	return awst::makeAsBiguint(awst::makeItob(std::move(lenU64), _loc), _loc);
}

void AssemblyBuilder::initCalldataPointerLocals(
	std::vector<std::shared_ptr<awst::Statement>>& _out, awst::SourceLocation const& _loc)
{
	for (auto const& [name, type]: m_calldataParams)
	{
		// STATIC calldata pointer param (struct / fixed array) referenced as a bare
		// pointer in this block: seed __cd_off_<name> with its constant data offset
		// (statics live inline in the head area — m_localConstants holds the byte pos).
		if (!isDynamicCalldataType(type))
		{
			if (!m_calldataStaticPtrNames.count(name)) continue;
			auto cdIt = m_localConstants.find(name);
			if (cdIt == m_localConstants.end()) continue;
			if (m_seededCalldataPointers)
			{
				if (m_seededCalldataPointers->count(name)) continue;
				m_seededCalldataPointers->insert(name);
			}
			_out.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression("__cd_off_" + name, awst::WType::biguintType(), _loc),
				awst::makeIntegerConstant(cdIt->second, _loc, awst::WType::biguintType()), _loc));
			continue;
		}
		auto cdIt = m_localConstants.find(name);
		if (cdIt == m_localConstants.end()) continue;
		// Seed ONCE per function: a later block must see a pointer mutated by an
		// earlier block (x.offset := V), not a fresh canonical re-seed.
		if (m_seededCalldataPointers)
		{
			if (m_seededCalldataPointers->count(name)) continue;
			m_seededCalldataPointers->insert(name);
		}
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression("__cd_off_" + name, awst::WType::biguintType(), _loc),
			calldataDynOffset(cdIt->second, _loc), _loc));
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression("__cd_len_" + name, awst::WType::biguintType(), _loc),
			calldataDynLength(cdIt->second, _loc), _loc));
	}
}

void AssemblyBuilder::buildSyntheticCalldataBlob(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	awst::SourceLocation const& _loc
)
{
	using O = awst::UInt64BinaryOperator;

	auto u64Const = [&](uint64_t v) {
		return awst::makeIntegerConstant(v, _loc, awst::WType::uint64Type());
	};
	auto bytesVar = [&](std::string const& n) {
		return awst::makeVarExpression(n, awst::WType::bytesType(), _loc);
	};
	auto u64Var = [&](std::string const& n) {
		return awst::makeVarExpression(n, awst::WType::uint64Type(), _loc);
	};
	auto bzeroOf = [&](std::shared_ptr<awst::Expression> n) {
		return awst::makeBzero(std::move(n), _loc);
	};
	auto concatBytes = [&](std::shared_ptr<awst::Expression> a, std::shared_ptr<awst::Expression> b) {
		return awst::makeConcat(std::move(a), std::move(b), _loc);
	};
	auto lenOf = [&](std::shared_ptr<awst::Expression> b) {
		return awst::makeLen(std::move(b), _loc);
	};

	// Layout: 4-byte selector + head area + tail. Head sizes are per-param
	// (solc calldataHeadSize: statics inline their FULL encoded size — a
	// `uint8[3]` occupies 96 head bytes, shifting every later param), not the
	// old one-word-per-param assumption. __cd_tail_off = running tail offset
	// (relative to args start = 0x04); starts at the total head size.
	std::vector<uint64_t> headSizes;
	uint64_t headTotal = 0;
	for (auto const& [pname, ptype]: _params)
	{
		headSizes.push_back(calldataHeadSizeOf(pname, ptype));
		headTotal += headSizes.back();
	}

	// __cd_blob selector slot: the RUNTIME selector that routed this call —
	// txna ApplicationArgs 0, the 4-byte sha512_256-based ARC-4 selector. AVM
	// selectors are sha512_256 BY DESIGN project-wide (router dispatch, encodeCall,
	// MethodConstant, ARC-28 events; accepted design divergence from EVM keccak —
	// only the Error/Panic revert magics stay EVM-literal), so asm reads of
	// calldata bytes 0-3 must see the SAME selector the router matched, not a
	// keccak value nothing else in the system uses. Guarded by NumAppArgs > 0
	// (bzero(4) during construction / bare calls where no args exist).
	auto numArgs = awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), _loc);
	auto hasArgs = awst::makeNumericCompare(
		std::move(numArgs), awst::NumericComparison::Gt, u64Const(0), _loc);
	auto selectorBytes = awst::makeConditional(
		std::move(hasArgs), awst::makeAppArg(0, _loc), bzeroOf(u64Const(4)),
		awst::WType::bytesType(), _loc);
	_out.push_back(awst::makeAssignmentStatement(
		bytesVar(CD_BLOB_VAR), std::move(selectorBytes), _loc));

	// __cd_tail_off = total head size — running offset of next tail entry
	_out.push_back(awst::makeAssignmentStatement(
		u64Var("__cd_tail_off"), u64Const(headTotal), _loc));

	// Pass 1: append head areas; pass 2: append tail bodies for dynamic params.
	for (size_t i = 0; i < _params.size(); ++i)
	{
		auto const& [name, type] = _params[i];
		if (isDynamicCalldataType(type))
		{
			// Head: current tail offset (updated in pass 2 after emitting the tail body).
			_out.push_back(awst::makeAssignmentStatement(
				bytesVar(CD_BLOB_VAR),
				concatBytes(bytesVar(CD_BLOB_VAR), pad32BE(u64Var("__cd_tail_off"), _loc)),
				_loc));
		}
		else
		{
			// Static head: per-leaf EVM words (sign-extended / right-aligned
			// per the declared solc type).
			_out.push_back(awst::makeAssignmentStatement(
				bytesVar(CD_BLOB_VAR),
				concatBytes(bytesVar(CD_BLOB_VAR), evmStaticHeadBytes(name, type, _loc)),
				_loc));
		}
	}

	// Head byte offset of param j within the blob: 4 + Σ headSizes[0..j).
	std::vector<uint64_t> headPos(_params.size(), 4);
	for (size_t j = 1; j < _params.size(); ++j)
		headPos[j] = headPos[j - 1] + headSizes[j - 1];

	// Tail pass: for each dynamic param, emit length word + EVM-encoded data,
	// then advance __cd_tail_off and patch the next dynamic head via replace3.
	for (size_t i = 0; i < _params.size(); ++i)
	{
		auto const& [name, type] = _params[i];
		if (!isDynamicCalldataType(type)) continue;

		int bodyId = awst::NameGen::next("SyntheticCalldata.body");
		std::string cntN = "__cd_cnt_" + std::to_string(bodyId);
		std::string bodyN = "__cd_body_" + std::to_string(bodyId);
		auto cntVar = [&]() { return u64Var(cntN); };
		auto bodyVar = [&]() { return bytesVar(bodyN); };

		// EVM length word: BYTE length for bytes/string, ELEMENT COUNT for
		// arrays (the ARC4 2-byte header — universal for any element size).
		auto var = awst::makeVarExpression(name, type, _loc);
		std::shared_ptr<awst::Expression> lenExpr;
		if (type == awst::WType::bytesType() || type == awst::WType::stringType())
			lenExpr = lenOf(std::move(var));
		else
			lenExpr = awst::makeBtoi(awst::makeExtract3(
				awst::makeAsBytes(std::move(var), _loc), u64Const(0), u64Const(2), _loc), _loc);
		_out.push_back(awst::makeAssignmentStatement(cntVar(), std::move(lenExpr), _loc));
		_out.push_back(awst::makeAssignmentStatement(
			bytesVar(CD_BLOB_VAR),
			concatBytes(bytesVar(CD_BLOB_VAR), pad32BE(cntVar(), _loc)),
			_loc));

		// Body → __cd_body_N. bytes/string: raw padded. Arrays whose ARC4
		// element bytes already equal the EVM element encoding (uint256[],
		// uint[2][]): strip the 2-byte header, pad. Otherwise (sub-word /
		// signed / bool elements): re-encode PER ELEMENT at runtime — each
		// leaf widened to its EVM 32-byte word (possible_solc item 2; the old
		// strip emitted 1-byte uint8[] elements where EVM has padded words).
		auto var2 = awst::makeVarExpression(name, type, _loc);
		bool isByteish = (type == awst::WType::bytesType() || type == awst::WType::stringType());
		awst::WType const* elemW = nullptr;
		if (auto const* dyn = dynamic_cast<awst::ARC4DynamicArray const*>(type))
			elemW = dyn->elementType();
		solidity::frontend::Type const* solElem = nullptr;
		if (auto const* solT = calldataSolType(name); solTypeUsable(solT))
			if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(solT))
				solElem = at->baseType();
		int arc4Elem = elemW ? computeARC4ByteSize(elemW) : 32;
		bool elemIsBool = elemW
			&& (elemW == awst::WType::boolType() || elemW->name() == "arc4.bool");
		bool reencode = !isByteish && elemW
			&& (elemIsBool || arc4Elem != 32)
			&& solElem && solElem->isValueType();
		if (isByteish)
			_out.push_back(awst::makeAssignmentStatement(bodyVar(),
				padTo32Multiple(awst::makeAsBytes(std::move(var2), _loc), _loc), _loc));
		else if (!reencode)
		{
			auto bytes = awst::makeAsBytes(std::move(var2), _loc);
			auto lenCall = awst::makeLen(bytes, _loc);
			auto sub2 = awst::makeUInt64BinOp(std::move(lenCall), O::Sub, u64Const(2), _loc);
			auto extract = awst::makeExtract3(std::move(bytes), u64Const(2), std::move(sub2), _loc);
			_out.push_back(awst::makeAssignmentStatement(bodyVar(),
				padTo32Multiple(std::move(extract), _loc), _loc));
		}
		else
		{
			std::string idxN = "__cd_i_" + std::to_string(bodyId);
			auto idxVar = [&]() { return u64Var(idxN); };
			_out.push_back(awst::makeAssignmentStatement(bodyVar(),
				awst::makeBytesConstant({}, _loc, awst::BytesEncoding::Unknown), _loc));
			_out.push_back(awst::makeAssignmentStatement(idxVar(), u64Const(0), _loc));
			auto body = awst::makeBlock(_loc);
			auto elem = awst::makeIndexExpression(std::move(var2), idxVar(), elemW, _loc);
			body->body.push_back(awst::makeAssignmentStatement(bodyVar(),
				concatBytes(bodyVar(), evmCalldataWord(std::move(elem), solElem, _loc)), _loc));
			body->body.push_back(awst::makeAssignmentStatement(idxVar(),
				awst::makeUInt64BinOp(idxVar(), O::Add, u64Const(1), _loc), _loc));
			auto cond = awst::makeNumericCompare(
				idxVar(), awst::NumericComparison::Lt, cntVar(), _loc);
			_out.push_back(awst::makeWhileLoop(std::move(cond), std::move(body), _loc));
		}
		_out.push_back(awst::makeAssignmentStatement(
			bytesVar(CD_BLOB_VAR),
			concatBytes(bytesVar(CD_BLOB_VAR), bodyVar()),
			_loc));

		// Advance tail offset: __cd_tail_off += 32 + len(body).
		auto advance = awst::makeUInt64BinOp(
			awst::makeUInt64BinOp(u64Var("__cd_tail_off"), O::Add, u64Const(32), _loc),
			O::Add, lenOf(bodyVar()), _loc);
		_out.push_back(awst::makeAssignmentStatement(u64Var("__cd_tail_off"), advance, _loc));

		// PATCH the next dynamic head (at its per-param head offset — statics
		// shift it) with the now-correct __cd_tail_off; subsequent iterations
		// chain the rest.
		for (size_t j = i + 1; j < _params.size(); ++j)
		{
			if (!isDynamicCalldataType(_params[j].second)) continue;
			auto patch = awst::makeReplace3(bytesVar(CD_BLOB_VAR),
				u64Const(headPos[j]), pad32BE(u64Var("__cd_tail_off"), _loc), _loc);
			_out.push_back(awst::makeAssignmentStatement(bytesVar(CD_BLOB_VAR), std::move(patch), _loc));
			break;
		}
	}

	// Register __cd_blob in m_locals so subsequent reads pick up its type.
	m_locals[CD_BLOB_VAR] = awst::WType::bytesType();
}

} // namespace puyasol::builder
