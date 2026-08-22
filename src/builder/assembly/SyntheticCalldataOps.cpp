/// @file SyntheticCalldataOps.cpp
/// Synthetic EVM-ABI calldata blob: when Yul accesses calldata at a non-constant
/// offset, stand up `__cd_blob` at the assembly-block entry so dynamic calldataload
/// becomes `extract3(__cd_blob, off, 32)`.
///
/// Layout + value widening are driven by the DECLARED solc types when available.
/// Head sizes come from `Type::calldataHeadSize()` (statics
/// inline their full encoded size in the head), signed sub-word params
/// sign-extend to the 32-byte word, static aggregates emit one word per leaf,
/// and sub-word-element dynamic arrays re-encode per element at runtime.
/// The blob is deliberately EVM-32-byte-word-shaped — ARC4 packing is the VALUE
/// transport; this is the offset-faithful view Yul arithmetic needs.

#include "builder/assembly/AssemblyBuilder.h"
#include "awst/NameGen.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "Logger.h"

#include <libsolidity/ast/Types.h>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

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
				// calldataload needs the synthetic blob for every non-constant
				// offset, every constant outside the statically mapped head, and
				// the head of a dynamically encoded parameter (that word is an ABI
				// offset, not the parameter's decoded value).
				// calldatacopy: non-const src or len → dynamic.
				// calldatasize: always runtime → dynamic (blob provides len(__cd_blob)).
				if (n == "calldatasize")
					found = true;
				else if (n == "calldataload" && call->arguments.size() == 1)
				{
					auto off = resolveConstantYulValue(call->arguments[0]);
					if (!off)
						found = true;
					else if (auto it = m_calldataMap.find(*off);
						it == m_calldataMap.end())
						found = true;
					else if (auto const* solType = calldataSolType(it->second.paramName);
						solTypeUsable(solType) && solType->isDynamicallyEncoded())
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
	if (wt == awst::WType::arc4BoolType())
	{
		auto decoded = awst::makeARC4Decode(
			std::move(_value), awst::WType::boolType(), _loc);
		auto castU64 = awst::makeAsUInt64(std::move(decoded), _loc);
		auto byteByVal = awst::makeItob(std::move(castU64), _loc);
		auto lastByte = awst::makeExtract3(
			std::move(byteByVal), u64c(7), u64c(1), _loc);
		return awst::makeConcat(
			awst::makeBzero(31, _loc), std::move(lastByte), _loc);
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
	if (arc4IsDynamic(_type)) return true;
	if (_type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(_type);
		return refArr && !refArr->arraySize().has_value();
	}
	return false;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::calldataDynOffset(
	uint64_t _headPos, solidity::frontend::Type const* _solType,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto u64 = [&](uint64_t v) { return awst::makeIntegerConstant(v, _loc, awst::WType::uint64Type()); };
	// headWord = btoi(extract3(__cd_blob, headPos+24, 8))  — low 8 bytes of the 32-byte head pointer.
	auto headWord = awst::makeBtoi(awst::makeExtract3(
		awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), _loc),
		u64(_headPos + 24), u64(8), _loc), _loc);
	// Dynamic arrays/bytes expose their first element/data byte after the count.
	// Fixed arrays and structs can be dynamically encoded because a nested member
	// is dynamic, but their tail has no outer count word.
	uint64_t prefix = 32;
	if (auto const* array = dynamic_cast<ArrayType const*>(_solType))
		prefix = array->isDynamicallySized() ? 32 : 0;
	else if (_solType)
		prefix = 0;
	auto offU64 = awst::makeUInt64BinOp(
		std::move(headWord), awst::UInt64BinaryOperator::Add,
		u64(4 + prefix), _loc);
	return awst::makeAsBiguint(awst::makeItob(std::move(offU64), _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::calldataDynLength(
	uint64_t _headPos, solidity::frontend::Type const* _solType,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto u64 = [&](uint64_t v) { return awst::makeIntegerConstant(v, _loc, awst::WType::uint64Type()); };
	if (auto const* array = dynamic_cast<ArrayType const*>(_solType);
		array && !array->isDynamicallySized())
		return awst::makeIntegerConstant(
			array->length().str(), _loc, awst::WType::biguintType());
	if (_solType && !dynamic_cast<ArrayType const*>(_solType))
		return awst::makeZero(_loc, awst::WType::biguintType());
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
		auto const* solType = calldataSolType(name);
		bool const dynamicallyEncoded = solTypeUsable(solType)
			? solType->isDynamicallyEncoded()
			: isDynamicCalldataType(type);
		// STATIC calldata pointer param (struct / fixed array) referenced as a bare
		// pointer in this block: seed __cd_off_<name> with its constant data offset
		// (statics live inline in the head area — m_localConstants holds the byte pos).
		if (!dynamicallyEncoded)
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
			calldataDynOffset(cdIt->second, solType, _loc), _loc));
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression("__cd_len_" + name, awst::WType::biguintType(), _loc),
			calldataDynLength(cdIt->second, solType, _loc), _loc));
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
	std::vector<bool> dynamicParams;
	uint64_t headTotal = 0;
	for (auto const& [pname, ptype]: _params)
	{
		headSizes.push_back(calldataHeadSizeOf(pname, ptype));
		auto const* solType = calldataSolType(pname);
		dynamicParams.push_back(solTypeUsable(solType)
			? solType->isDynamicallyEncoded()
			: isDynamicCalldataType(ptype));
		headTotal += headSizes.back();
	}

	// __cd_blob selector slot starts with the runtime ARC-4 selector. The opt-in
	// selector policy translates known routes to their Solidity keccak selector
	// before inline assembly observes calldata bytes 0..3. Guarded by
	// NumAppArgs > 0 (bzero(4) during construction / bare calls).
	auto numArgs = awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), _loc);
	auto hasArgs = awst::makeNumericCompare(
		std::move(numArgs), awst::NumericComparison::Gt, u64Const(0), _loc);
	std::shared_ptr<awst::Expression> selectorBytes = awst::makeConditional(
		std::move(hasArgs), awst::makeAppArg(0, _loc), bzeroOf(u64Const(4)),
		awst::WType::bytesType(), _loc);
	if (m_typeMapper.profile().evmSelectors)
		selectorBytes = SelectorSemantics::translateRuntimeSelector(
			std::move(selectorBytes), m_selectorRoutes, _loc);
	_out.push_back(awst::makeAssignmentStatement(
		bytesVar(CD_BLOB_VAR), std::move(selectorBytes), _loc));

	// __cd_tail_off = total head size — running offset of next tail entry
	_out.push_back(awst::makeAssignmentStatement(
		u64Var("__cd_tail_off"), u64Const(headTotal), _loc));

	// Pass 1: append head areas; pass 2: append tail bodies for dynamic params.
	for (size_t i = 0; i < _params.size(); ++i)
	{
		auto const& [name, type] = _params[i];
		if (dynamicParams[i])
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

	// Recursively rebuild one complete EVM dynamic tail (length word + body).
	// ARC-4 uses uint16 counts and uint16 offsets at every dynamic nesting level;
	// EVM uses 32-byte counts/offsets and a separate head/tail layout. Keeping
	// this recursive is important: T[][][] is the same problem as T[][], not a
	// distinct special case.
	std::function<std::shared_ptr<awst::Expression>(
		std::shared_ptr<awst::Expression>, awst::WType const*,
		solidity::frontend::Type const*,
		std::vector<std::shared_ptr<awst::Statement>>&)> encodeDynamicTail;
	encodeDynamicTail = [&](std::shared_ptr<awst::Expression> value,
		awst::WType const* valueW,
		solidity::frontend::Type const* solType,
		std::vector<std::shared_ptr<awst::Statement>>& stmts)
		-> std::shared_ptr<awst::Expression>
	{
		int id = awst::NameGen::next("SyntheticCalldata.dynamicTail");
		auto suffix = std::to_string(id);
		std::string valueN = "__cd_dyn_value_" + suffix;
		std::string encodedN = "__cd_dyn_encoded_" + suffix;
		std::string cntN = "__cd_dyn_count_" + suffix;
		std::string bodyN = "__cd_dyn_body_" + suffix;
		std::string tailN = "__cd_dyn_tail_" + suffix;
		auto valueVar = [&] {
			return awst::makeVarExpression(valueN, valueW, _loc);
		};
		auto encodedVar = [&] { return bytesVar(encodedN); };
		auto cntVar = [&] { return u64Var(cntN); };
		auto bodyVar = [&] { return bytesVar(bodyN); };
		auto tailVar = [&] { return bytesVar(tailN); };
		auto emptyBytes = [&] {
			return awst::makeBytesConstant(
				{}, _loc, awst::BytesEncoding::Unknown);
		};

		stmts.push_back(awst::makeAssignmentStatement(
			valueVar(), std::move(value), _loc));
		auto const* solArray =
			dynamic_cast<solidity::frontend::ArrayType const*>(solType);
		auto const* solStruct =
			dynamic_cast<solidity::frontend::StructType const*>(solType);
		bool const byteish = solArray && solArray->isByteArrayOrString();
		bool const nativeByteish = valueW == awst::WType::bytesType()
			|| valueW == awst::WType::stringType();
		stmts.push_back(awst::makeAssignmentStatement(
			encodedVar(), awst::makeAsBytes(valueVar(), _loc), _loc));

		if (byteish && nativeByteish)
		{
			stmts.push_back(awst::makeAssignmentStatement(
				cntVar(), lenOf(encodedVar()), _loc));
			stmts.push_back(awst::makeAssignmentStatement(
				bodyVar(), padTo32Multiple(encodedVar(), _loc), _loc));
		}
		else if (solArray)
		{
			// Dynamically-sized ARC-4 arrays carry a uint16 count. Fixed arrays
			// take their count from solc's declared type and carry no prefix.
			if (solArray->isDynamicallySized())
				stmts.push_back(awst::makeAssignmentStatement(cntVar(),
					awst::makeBtoi(awst::makeExtract3(
						encodedVar(), u64Const(0), u64Const(2), _loc), _loc), _loc));
			else
				stmts.push_back(awst::makeAssignmentStatement(
					cntVar(), u64Const(solArray->length().convert_to<uint64_t>()), _loc));
			if (byteish)
			{
				auto rawLen = awst::makeUInt64BinOp(
					lenOf(encodedVar()), O::Sub, u64Const(2), _loc);
				auto raw = awst::makeExtract3(
					encodedVar(), u64Const(2), std::move(rawLen), _loc);
				stmts.push_back(awst::makeAssignmentStatement(
					bodyVar(), padTo32Multiple(std::move(raw), _loc), _loc));
			}
			else
			{
				auto const* elemSol = solArray ? solArray->baseType() : nullptr;
				auto const* elemW = arrayElementWtype(valueW);
					std::string idxN = "__cd_dyn_i_" + suffix;
				auto idxVar = [&] { return u64Var(idxN); };
				stmts.push_back(awst::makeAssignmentStatement(
					bodyVar(), emptyBytes(), _loc));
				stmts.push_back(awst::makeAssignmentStatement(
					idxVar(), u64Const(0), _loc));
				auto loopBody = awst::makeBlock(_loc);

					if (elemSol && elemSol->isDynamicallyEncoded())
				{
					std::string tailsN = "__cd_dyn_tails_" + suffix;
					std::string offN = "__cd_dyn_offset_" + suffix;
					std::string elemN = "__cd_dyn_elem_" + suffix;
					auto tailsVar = [&] { return bytesVar(tailsN); };
					auto offVar = [&] { return u64Var(offN); };
					auto elemVar = [&] {
						return awst::makeVarExpression(elemN, elemW, _loc);
					};
					stmts.push_back(awst::makeAssignmentStatement(
						tailsVar(), emptyBytes(), _loc));
						stmts.push_back(awst::makeAssignmentStatement(offVar(),
							awst::makeUInt64BinOp(
								cntVar(), O::Mult,
								u64Const(elemSol->calldataHeadSize()), _loc), _loc));
					loopBody->body.push_back(awst::makeAssignmentStatement(
						elemVar(), awst::makeIndexExpression(
							valueVar(), idxVar(), elemW, _loc), _loc));
					loopBody->body.push_back(awst::makeAssignmentStatement(
						bodyVar(), concatBytes(
							bodyVar(), pad32BE(offVar(), _loc)), _loc));
						auto elemTail = encodeDynamicTail(
							elemVar(), elemW, elemSol, loopBody->body);
					loopBody->body.push_back(awst::makeAssignmentStatement(
						tailsVar(), concatBytes(tailsVar(), elemTail), _loc));
					loopBody->body.push_back(awst::makeAssignmentStatement(
						offVar(), awst::makeUInt64BinOp(
							offVar(), O::Add, lenOf(elemTail), _loc), _loc));
					loopBody->body.push_back(awst::makeAssignmentStatement(
						idxVar(), awst::makeUInt64BinOp(
							idxVar(), O::Add, u64Const(1), _loc), _loc));
					auto cond = awst::makeNumericCompare(
						idxVar(), awst::NumericComparison::Lt, cntVar(), _loc);
					stmts.push_back(awst::makeWhileLoop(
						std::move(cond), std::move(loopBody), _loc));
					stmts.push_back(awst::makeAssignmentStatement(
						bodyVar(), concatBytes(bodyVar(), tailsVar()), _loc));
				}
				else
				{
					// A non-dynamic element may still be a static aggregate. Emit
					// all of its scalar EVM words rather than relying on ARC-4's
					// backing width/layout.
					auto elem = awst::makeIndexExpression(
						valueVar(), idxVar(), elemW, _loc);
					std::vector<std::shared_ptr<awst::Expression>> words;
					emitEvmHeadWords(elem, elemW, elemSol, words, _loc);
					for (auto& word: words)
						loopBody->body.push_back(awst::makeAssignmentStatement(
							bodyVar(), concatBytes(bodyVar(), std::move(word)), _loc));
					loopBody->body.push_back(awst::makeAssignmentStatement(
						idxVar(), awst::makeUInt64BinOp(
							idxVar(), O::Add, u64Const(1), _loc), _loc));
					auto cond = awst::makeNumericCompare(
						idxVar(), awst::NumericComparison::Lt, cntVar(), _loc);
					stmts.push_back(awst::makeWhileLoop(
						std::move(cond), std::move(loopBody), _loc));
					}
				}
			}
			else if (solStruct)
			{
				// A dynamic struct is a tuple: solc supplies the exact internal
				// head size, and each dynamic member contributes an offset plus a
				// recursively encoded tail.
				std::string tailsN = "__cd_dyn_tails_" + suffix;
				std::string offN = "__cd_dyn_offset_" + suffix;
				auto tailsVar = [&] { return bytesVar(tailsN); };
				auto offVar = [&] { return u64Var(offN); };
				stmts.push_back(awst::makeAssignmentStatement(
					bodyVar(), emptyBytes(), _loc));
				stmts.push_back(awst::makeAssignmentStatement(
					tailsVar(), emptyBytes(), _loc));
				stmts.push_back(awst::makeAssignmentStatement(
					offVar(), u64Const(solStruct->calldataEncodedTailSize()), _loc));
				auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(valueW);
				auto const& members = solStruct->structDefinition().members();
				for (size_t i = 0; i < members.size(); ++i)
				{
					auto const* memberSol = members[i]->type();
					auto const* memberW = arc4Struct && i < arc4Struct->fields().size()
						? arc4Struct->fields()[i].second : nullptr;
					auto member = awst::makeFieldExpression(
						valueVar(), members[i]->name(), memberW, _loc);
					if (memberSol->isDynamicallyEncoded())
					{
						stmts.push_back(awst::makeAssignmentStatement(
							bodyVar(), concatBytes(
								bodyVar(), pad32BE(offVar(), _loc)), _loc));
						auto memberTail = encodeDynamicTail(
							std::move(member), memberW, memberSol, stmts);
						stmts.push_back(awst::makeAssignmentStatement(
							tailsVar(), concatBytes(tailsVar(), memberTail), _loc));
						stmts.push_back(awst::makeAssignmentStatement(
							offVar(), awst::makeUInt64BinOp(
								offVar(), O::Add, lenOf(memberTail), _loc), _loc));
					}
					else
					{
						std::vector<std::shared_ptr<awst::Expression>> words;
						emitEvmHeadWords(
							std::move(member), memberW, memberSol, words, _loc);
						for (auto& word: words)
							stmts.push_back(awst::makeAssignmentStatement(
								bodyVar(), concatBytes(
									bodyVar(), std::move(word)), _loc));
					}
				}
				stmts.push_back(awst::makeAssignmentStatement(
					bodyVar(), concatBytes(bodyVar(), tailsVar()), _loc));
			}
			else
			{
				Logger::instance().error(
					"unsupported dynamically encoded Solidity calldata type",
					_loc);
				stmts.push_back(awst::makeAssignmentStatement(
					bodyVar(), emptyBytes(), _loc));
			}

			std::shared_ptr<awst::Expression> complete = bodyVar();
			if (solArray && solArray->isDynamicallySized())
				complete = concatBytes(pad32BE(cntVar(), _loc), std::move(complete));
			stmts.push_back(awst::makeAssignmentStatement(
				tailVar(), std::move(complete), _loc));
		return tailVar();
	};

	// Tail pass: for each dynamic param, emit its recursively EVM-encoded tail,
	// then advance __cd_tail_off and patch the next dynamic head via replace3.
	for (size_t i = 0; i < _params.size(); ++i)
	{
		auto const& [name, type] = _params[i];
		if (!dynamicParams[i]) continue;
		auto const* solType = calldataSolType(name);
		if (!solTypeUsable(solType))
		{
			Logger::instance().error(
				"cannot derive EVM calldata tail layout for dynamic parameter",
				_loc);
			continue;
		}
		auto tail = encodeDynamicTail(
			awst::makeVarExpression(name, type, _loc), type, solType, _out);
		_out.push_back(awst::makeAssignmentStatement(
			bytesVar(CD_BLOB_VAR),
			concatBytes(bytesVar(CD_BLOB_VAR), tail),
			_loc));

		// Advance tail offset by the complete length-word + body tail.
		auto advance = awst::makeUInt64BinOp(
			u64Var("__cd_tail_off"), O::Add, lenOf(tail), _loc);
		_out.push_back(awst::makeAssignmentStatement(u64Var("__cd_tail_off"), advance, _loc));

		// PATCH the next dynamic head (at its per-param head offset — statics
		// shift it) with the now-correct __cd_tail_off; subsequent iterations
		// chain the rest.
		for (size_t j = i + 1; j < _params.size(); ++j)
		{
			if (!dynamicParams[j]) continue;
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
