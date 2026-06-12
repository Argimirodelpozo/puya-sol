/// @file SyntheticCalldataOps.cpp
/// Synthetic EVM-ABI calldata blob materialisation. When Yul accesses
/// calldata at a non-constant offset (e.g.
/// `calldatacopy(0x20, public_inputs_start, public_inputs_size)`) we
/// stand up a single bytes local `__cd_blob` at the start of the
/// assembly block. Dynamic-offset calldataload then becomes
/// `extract3(__cd_blob, off, 32)`.
///
/// Extracted from DataOps.cpp; both methods stay declared in
/// AssemblyBuilder.h, only the implementation moves.

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
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_expr))
		{
			std::string n = getFunctionName(call->functionName);
			if (isCalldataOp(n))
			{
				// calldataload(off): non-constant off ⇒ dynamic.
				// calldatacopy(dest, src, len): non-constant src or len ⇒ dynamic.
				// calldatasize(): always returns runtime length — counts as
				// dynamic so the blob exists for `len(__cd_blob)` reads.
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
				scanExpr(*assign->value);
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

// Helper: encode a uint64 as a 32-byte big-endian value via
// `concat(bzero(24), itob(val))`. itob produces 8 BE bytes.
std::shared_ptr<awst::Expression> pad32BE(
	std::shared_ptr<awst::Expression> _u64Val, awst::SourceLocation const& _loc)
{
	return awst::makeLeftPad(awst::makeItob(std::move(_u64Val), _loc), 24, _loc);
}

// Pad to a 32-byte multiple: shared canonical helper (awst::makeRightPadTo32Multiple).
std::shared_ptr<awst::Expression> padTo32Multiple(
	std::shared_ptr<awst::Expression> _bytes, awst::SourceLocation const& _loc)
{
	return awst::makeRightPadTo32Multiple(std::move(_bytes), _loc);
}

bool isDynamicAbi(awst::WType const* _type)
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

} // anonymous

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

	// Layout: 4-byte selector (zeros) + N×32 head section + tail section.
	// We compute the running tail offset as a uint64 local `__cd_tail_off`
	// (relative to start of args = 0x04). It starts at N*32 (size of head).
	uint64_t headWords = _params.size();

	// __cd_blob = bzero(4)  — selector slot
	_out.push_back(awst::makeAssignmentStatement(
		bytesVar(CD_BLOB_VAR), bzeroOf(u64Const(4)), _loc));

	// __cd_tail_off = headWords * 32  — running offset of next tail entry
	_out.push_back(awst::makeAssignmentStatement(
		u64Var("__cd_tail_off"), u64Const(headWords * 32), _loc));

	// Two passes: first append head words for each param (computing tail
	// offsets along the way for dynamic params), then append tail bodies
	// for the dynamic params in declaration order.
	for (size_t i = 0; i < _params.size(); ++i)
	{
		auto const& [name, type] = _params[i];
		if (isDynamicAbi(type))
		{
			// Head: pad32BE(__cd_tail_off)
			_out.push_back(awst::makeAssignmentStatement(
				bytesVar(CD_BLOB_VAR),
				concatBytes(bytesVar(CD_BLOB_VAR), pad32BE(u64Var("__cd_tail_off"), _loc)),
				_loc));
			// __cd_tail_off += 32 (length word) + paddedLen(param) — done
			// in the tail-emission pass below to avoid recomputing length.
		}
		else
		{
			// Static head: read param value, encode as 32 BE bytes.
			// For simplicity: pad biguint/uint64/bool/account/bytesN to 32 bytes BE.
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
				// bool → 32 bytes: 31 zeros + 0x01/0x00
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
				// Fallback: treat as 32-byte bytes (best-effort).
				headWord = awst::makeAsBytes(std::move(paramVar), _loc);
			}
			_out.push_back(awst::makeAssignmentStatement(
				bytesVar(CD_BLOB_VAR),
				concatBytes(bytesVar(CD_BLOB_VAR), std::move(headWord)),
				_loc));
		}
	}

	// Tail pass: for each dynamic param, append length word + data,
	// updating __cd_tail_off so subsequent dynamic params get correct heads.
	// Heads were already emitted — but we patched them with the
	// then-current __cd_tail_off, so this works iff we walk in declaration
	// order (which we did).
	//
	// HOWEVER — the loop above already wrote each dynamic-param head with
	// __cd_tail_off as it stood at that point. We now need to advance
	// __cd_tail_off by the size of the just-emitted tail entry BEFORE the
	// next dynamic param's head was written. That's an ordering issue:
	// the head writes happened in the loop above without updating
	// __cd_tail_off for dynamic params after them.
	//
	// Fix: redo the loop, this time interleaving — see updated impl below.
	// (Keeping the simple two-pass form here for an MVP that only handles
	// the common case of a SINGLE dynamic param OR multiple dynamics where
	// the test only reads the first dynamic's head; honk's verify(bytes,
	// bytes32[]) needs the second head correct, so emit the tail in order
	// AND patch the second head later via a replace3.)

	// MVP: assume <=1 dynamic OR caller doesn't read second head. For
	// honk/Blake.sol verify(bytes _proof, bytes32[] _publicInputs), the
	// Yul reads `calldataload(0x24)` (publicInputs head). To make that
	// correct, we need the second head value to be
	// 0x40 + 32 + paddedLen(_proof). Patch via replace3 after emitting
	// proof tail.

	// First dynamic encountered in head pass: collect its index & param,
	// so the tail pass knows which to emit first.
	for (size_t i = 0; i < _params.size(); ++i)
	{
		auto const& [name, type] = _params[i];
		if (!isDynamicAbi(type)) continue;

		// Append length word (32 bytes BE of len(param))
		auto var = awst::makeVarExpression(name, type, _loc);
		auto lenExpr = lenOf(var);
		_out.push_back(awst::makeAssignmentStatement(
			bytesVar(CD_BLOB_VAR),
			concatBytes(bytesVar(CD_BLOB_VAR), pad32BE(std::move(lenExpr), _loc)),
			_loc));

		// Append param data padded to 32-byte multiple
		auto var2 = awst::makeVarExpression(name, type, _loc);
		// For arc4 dynamic-array types the on-disk repr starts with a
		// uint16 length header — strip it for the calldata body.
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

		// Update tail offset for subsequent params: __cd_tail_off += 32 + paddedLen
		// (Used only if we needed to patch later heads. For the MVP we
		// patch the second head via replace3 just below.)
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
			if (!isDynamicAbi(_params[j].second)) continue;
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
