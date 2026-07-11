/// @file SyntheticCalldataOps.cpp
/// Synthetic EVM-ABI calldata blob: when Yul accesses calldata at a non-constant
/// offset, stand up `__cd_blob` at the assembly-block entry so dynamic calldataload
/// becomes `extract3(__cd_blob, off, 32)`.

#include "builder/assembly/AssemblyBuilder.h"

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
					if (!resolveConstantYulValue(call->arguments[1])
						|| !resolveConstantYulValue(call->arguments[2]))
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

} // anonymous

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

	// Layout: 4-byte selector (zeros) + N×32 head + tail.
	// __cd_tail_off = running tail offset (relative to args start = 0x04); starts at N*32.
	uint64_t headWords = _params.size();

	// __cd_blob selector slot: the REAL 4-byte EVM keccak selector when known
	// (so asm reads of bytes 0-3 — calldataload(0), a repointed x.offset := 0 —
	// see what EVM would show); bzero(4) otherwise (internal fns, constructors).
	std::shared_ptr<awst::Expression> selectorBytes;
	if (m_evmSelector.size() == 4)
		selectorBytes = awst::makeBytesConstant(
			std::vector<unsigned char>(m_evmSelector.begin(), m_evmSelector.end()),
			_loc, awst::BytesEncoding::Base16, awst::WType::bytesType());
	else
		selectorBytes = bzeroOf(u64Const(4));
	_out.push_back(awst::makeAssignmentStatement(
		bytesVar(CD_BLOB_VAR), std::move(selectorBytes), _loc));

	// __cd_tail_off = headWords * 32  — running offset of next tail entry
	_out.push_back(awst::makeAssignmentStatement(
		u64Var("__cd_tail_off"), u64Const(headWords * 32), _loc));

	// Pass 1: append head words; pass 2: append tail bodies for dynamic params.
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
			// Static head: encode as 32 BE bytes.
			auto paramVar = awst::makeVarExpression(name, type, _loc);
			std::shared_ptr<awst::Expression> headWord;
			if (type == awst::WType::uint64Type())
				headWord = pad32BE(std::move(paramVar), _loc);
			else if (type == awst::WType::biguintType())
			{
				// biguint as bytes; left-pad to 32 if shorter.
				auto orOp = awst::makeBytesOr(
					awst::makeAsBytes(std::move(paramVar), _loc),
					bzeroOf(u64Const(32)), _loc);
				headWord = std::move(orOp);
			}
			else if (type == awst::WType::boolType())
			{
				// 31 zero bytes + low byte of bool.
				auto bz = bzeroOf(u64Const(31));
				auto castU64 = awst::makeAsUInt64(std::move(paramVar), _loc);
				auto byteByVal = awst::makeItob(std::move(castU64), _loc);
				// itob produces 8 bytes BE; take last byte
				auto extract = awst::makeExtract3(std::move(byteByVal), u64Const(7), u64Const(1), _loc);
				headWord = concatBytes(std::move(bz), std::move(extract));
			}
			else if (type == awst::WType::accountType())
			{
				headWord = awst::makeAsBytes(std::move(paramVar), _loc);
			}
			else
			{
				headWord = awst::makeAsBytes(std::move(paramVar), _loc); // best-effort
			}
			_out.push_back(awst::makeAssignmentStatement(
				bytesVar(CD_BLOB_VAR),
				concatBytes(bytesVar(CD_BLOB_VAR), std::move(headWord)),
				_loc));
		}
	}

	// Tail pass: for each dynamic param, emit length word + padded data, then
	// advance __cd_tail_off and patch any subsequent dynamic heads via replace3.
	// MVP: handles <=1 dynamic OR multiple where subsequent heads get patched
	// (e.g. honk verify(bytes, bytes32[]) needs the second head = 0x40+32+paddedLen(proof)).
	for (size_t i = 0; i < _params.size(); ++i)
	{
		auto const& [name, type] = _params[i];
		if (!isDynamicCalldataType(type)) continue;

		auto var = awst::makeVarExpression(name, type, _loc);
		// EVM length word: BYTE length for bytes/string, ELEMENT COUNT for arrays.
		// An ARC4 dynamic array carries its element count in its 2-byte header —
		// read that (universal for any element size) instead of len(bytes), which
		// gave the byte length (2 + n*elemSize) and broke `l := x.length` on
		// `uint[2][] calldata` (130 instead of 2).
		std::shared_ptr<awst::Expression> lenExpr;
		if (type == awst::WType::bytesType() || type == awst::WType::stringType())
			lenExpr = lenOf(std::move(var));
		else
			lenExpr = awst::makeBtoi(awst::makeExtract3(
				awst::makeAsBytes(std::move(var), _loc), u64Const(0), u64Const(2), _loc), _loc);
		_out.push_back(awst::makeAssignmentStatement(
			bytesVar(CD_BLOB_VAR),
			concatBytes(bytesVar(CD_BLOB_VAR), pad32BE(std::move(lenExpr), _loc)),
			_loc));

		// Param data padded to 32-byte multiple.
		// ARC4 dynamic arrays have a 2-byte length header; strip it for calldata.
		auto var2 = awst::makeVarExpression(name, type, _loc);
		std::shared_ptr<awst::Expression> body;
		if (type == awst::WType::bytesType() || type == awst::WType::stringType())
			body = awst::makeAsBytes(std::move(var2), _loc);
		else
		{
			// ARC4 dynamic array: extract everything after the 2-byte length header.
			auto bytes = awst::makeAsBytes(std::move(var2), _loc);
			auto lenCall = awst::makeLen(bytes, _loc);
			auto sub2 = awst::makeUInt64BinOp(
				std::move(lenCall), O::Sub, u64Const(2), _loc);
			auto extract = awst::makeExtract3(std::move(bytes), u64Const(2), std::move(sub2), _loc);
			body = std::move(extract);
		}
		auto paddedBody = padTo32Multiple(std::move(body), _loc);
		_out.push_back(awst::makeAssignmentStatement(
			bytesVar(CD_BLOB_VAR),
			concatBytes(bytesVar(CD_BLOB_VAR), std::move(paddedBody)),
			_loc));

		// Advance tail offset: __cd_tail_off += 32 + paddedLen(param).
		auto var3 = awst::makeVarExpression(name, type, _loc);
		auto rawLen = lenOf(var3);
		auto modVal = awst::makeUInt64BinOp(
			rawLen, O::Mod, u64Const(32), _loc);
		auto padBytes = awst::makeUInt64BinOp(
			awst::makeUInt64BinOp(u64Const(32), O::Sub, std::move(modVal), _loc),
			O::Mod, u64Const(32), _loc);
		auto var4 = awst::makeVarExpression(name, type, _loc);
		auto rawLen2 = lenOf(var4);
		auto paddedLen = awst::makeUInt64BinOp(
			std::move(rawLen2), O::Add, std::move(padBytes), _loc);

		auto advance = awst::makeUInt64BinOp(
			awst::makeUInt64BinOp(u64Var("__cd_tail_off"), O::Add, u64Const(32), _loc),
			O::Add, std::move(paddedLen), _loc);
		_out.push_back(awst::makeAssignmentStatement(u64Var("__cd_tail_off"), advance, _loc));

		// PATCH later dynamic heads with the now-correct __cd_tail_off.
		// Each later dynamic param's head sits at byte offset
		// 4 + (its_index * 32) within the blob. Overwrite with the
		// current __cd_tail_off (which still points at the NEXT tail
		// entry, i.e. exactly the value that head should hold).
		for (size_t j = i + 1; j < _params.size(); ++j)
		{
			if (!isDynamicCalldataType(_params[j].second)) continue;
			uint64_t headByteOffset = 4 + j * 32;
			auto patch = awst::makeReplace3(bytesVar(CD_BLOB_VAR), u64Const(headByteOffset), pad32BE(u64Var("__cd_tail_off"), _loc), _loc);
			_out.push_back(awst::makeAssignmentStatement(bytesVar(CD_BLOB_VAR), std::move(patch), _loc));
			break;  // only patch the very next dynamic; updating
			        // __cd_tail_off in subsequent iterations chains them.
		}
	}

	// Register __cd_blob in m_locals so subsequent reads pick up its type.
	m_locals[CD_BLOB_VAR] = awst::WType::bytesType();
}

} // namespace puyasol::builder
