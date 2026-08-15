#include "builder/SourceLocConvert.h"
#include "builder/ProgramAnalysis.h"
#include <variant>
#include "builder/contract/ContractBuilder.h"
#include "awst/NameGen.h"
#include "awst/Visit.h"
#include "builder/NatSpecTags.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/BuildArtifacts.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>
#include <libyul/AST.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <map>
#include <set>

namespace puyasol::builder
{

/// Checks if a Solidity AST subtree references any state variable whose AST ID
/// is in the given set (i.e. box-stored state variables).

/// Collects local variable declarations inside a statement subtree (e.g. a
/// modifier body) so the inliner can rename them uniquely per application.
/// Without this, `modifier mod(uint x) { uint b = x; _; assert(b == x); }`
/// applied twice shares a single `b` slot across both instances.

/// Collects AST IDs of base functions that are called via `super.method()`.
/// These need to be emitted as separate subroutines with distinct names.

ContractBuilder::ContractBuilder(
	TypeMapper& _typeMapper,
	StorageMapper& _storageMapper,
	eb::FunctionPointerRegistry& _functionPointers,
	std::string const& _sourceFile,
	FunctionSymbolTable const& _functionSymbols,
	uint64_t _opupBudget,
	std::map<std::string, uint64_t> const& _ensureBudget,
	bool _viaIR,
	std::vector<solidity::frontend::FunctionDefinition const*> const& _hostBoundFunctions
)
	: m_typeMapper(_typeMapper),
	  m_storageMapper(_storageMapper),
	  m_functionPointers(_functionPointers),
	  m_sourceFile(_sourceFile),
	  m_functionSymbols(_functionSymbols),
	  m_opupBudget(_opupBudget),
	  m_ensureBudget(_ensureBudget),
	  m_viaIR(_viaIR),
	  m_hostBoundFunctions(_hostBoundFunctions)
{
}

// Free functions shared by AWSTBuilder (library/free-function path) and
// ContractBuilder (contract-method path).

awst::SourceLocation makeLoc(
	TypeMapper const& _typeMapper,
	std::string const& _sourceFile,
	solidity::langutil::SourceLocation const& _solLoc)
{
	return _typeMapper.sourceMap().toAwstLoc(_sourceFile, _solLoc);
}

namespace {

/// Collect decl IDs of memory aggregate locals referenced as VALUES in any
/// inline-assembly block in a function body. In Yul such a reference is the
/// aggregate's memory pointer (a uint256 offset), so we promote these to
/// blob-backed (SolVariableDeclaration) and resolve them to a uint64 offset in
/// the assembly translator.
// True iff `_vd` (a local) is initialised by `new T(...)` — walk its scope block
// for the declaring statement. Used to gate blob-backing of bytes/string asm
// buffers: only a freshly-`new`ed buffer (the OZ Strings.toString idiom) is
// promoted to the memory-pointer model; a bytes/string VALUE used in asm
// (`ret := val`) stays value-model.
static bool _isNewAllocatedLocal(solidity::frontend::VariableDeclaration const* _vd)
{
	using namespace solidity::frontend;
	auto const* block = dynamic_cast<Block const*>(_vd->scope());
	if (!block)
		return false;
	for (auto const& stmt: block->statements())
	{
		auto const* vds = dynamic_cast<VariableDeclarationStatement const*>(stmt.get());
		if (!vds || !vds->initialValue())
			continue;
		bool declares = false;
		for (auto const& d: vds->declarations())
			if (d && d->id() == _vd->id())
				declares = true;
		if (!declares)
			continue;
		auto const* fc = dynamic_cast<FunctionCall const*>(vds->initialValue());
		return fc && dynamic_cast<NewExpression const*>(&fc->expression()) != nullptr;
	}
	return false;
}

// True iff any of `_targets` (Yul identifier nodes referencing the buffer)
// appears inside `_e` (a Yul expression subtree).
static bool _yulExprRefs(
	solidity::yul::Expression const& _e,
	std::set<solidity::yul::Identifier const*> const& _targets)
{
	using namespace solidity::yul;
	if (auto const* id = std::get_if<Identifier>(&_e))
		return _targets.count(id) != 0;
	if (auto const* fc = std::get_if<FunctionCall>(&_e))
		for (auto const& arg: fc->arguments)
			if (_yulExprRefs(arg, _targets))
				return true;
	return false;
}

// True iff the buffer's pointer ESCAPES into another Yul variable within `_b` —
// i.e. it feeds the RHS of an assignment (`ptr := add(buffer, k)`) or a `let`.
// This is the exact signal that the value-model store handlers (mstore/mstore8
// with the buffer directly in the address, e.g. `mstore(add(x,32), w)`) can NOT
// cover the writes: once the pointer lives in an opaque local, only the blob
// (memory-pointer) model tracks it. A buffer used solely as a direct store
// address is left value-model. Recurses into nested control-flow blocks.
static bool _yulBlockEscapes(
	solidity::yul::Block const& _b,
	std::set<solidity::yul::Identifier const*> const& _targets)
{
	using namespace solidity::yul;
	for (auto const& stmt: _b.statements)
	{
		bool esc = std::visit([&](auto const& s) -> bool {
			using T = std::decay_t<decltype(s)>;
			if constexpr (std::is_same_v<T, Assignment>)
				return s.value && _yulExprRefs(*s.value, _targets);
			else if constexpr (std::is_same_v<T, VariableDeclaration>)
				return s.value && _yulExprRefs(*s.value, _targets);
			else if constexpr (std::is_same_v<T, Block>)
				return _yulBlockEscapes(s, _targets);
			else if constexpr (std::is_same_v<T, If>)
				return _yulBlockEscapes(s.body, _targets);
			else if constexpr (std::is_same_v<T, Switch>)
			{
				for (auto const& c: s.cases)
					if (_yulBlockEscapes(c.body, _targets))
						return true;
				return false;
			}
			else if constexpr (std::is_same_v<T, ForLoop>)
				return _yulBlockEscapes(s.pre, _targets)
					|| _yulBlockEscapes(s.post, _targets)
					|| _yulBlockEscapes(s.body, _targets);
			else if constexpr (std::is_same_v<T, FunctionDefinition>)
				return _yulBlockEscapes(s.body, _targets);
			else
				return false;
		}, stmt);
		if (esc)
			return true;
	}
	return false;
}

// True iff `_vd`'s memory pointer escapes into a Yul local inside `_asm`.
static bool _bufferPointerEscapes(
	solidity::frontend::InlineAssembly const& _asm,
	solidity::frontend::VariableDeclaration const* _vd)
{
	std::set<solidity::yul::Identifier const*> targets;
	for (auto const& ref: _asm.annotation().externalReferences)
		if (ref.second.declaration == _vd)
			targets.insert(ref.first);
	if (targets.empty())
		return false;
	return _yulBlockEscapes(_asm.operations().root(), targets);
}

// Collect the Identifiers that are ASSIGNMENT TARGETS (`x := …`) anywhere in a
// Yul block, including nested control flow.
static void _yulAssignTargets(
	solidity::yul::Block const& _b,
	std::set<solidity::yul::Identifier const*>& _out)
{
	using namespace solidity::yul;
	for (auto const& stmt: _b.statements)
		std::visit([&](auto const& s) {
			using T = std::decay_t<decltype(s)>;
			if constexpr (std::is_same_v<T, Assignment>)
				for (auto const& n: s.variableNames)
					_out.insert(&n);
			else if constexpr (std::is_same_v<T, Block>)
				_yulAssignTargets(s, _out);
			else if constexpr (std::is_same_v<T, If>)
				_yulAssignTargets(s.body, _out);
			else if constexpr (std::is_same_v<T, Switch>)
				for (auto const& c: s.cases)
					_yulAssignTargets(c.body, _out);
			else if constexpr (std::is_same_v<T, ForLoop>)
			{
				_yulAssignTargets(s.pre, _out);
				_yulAssignTargets(s.post, _out);
				_yulAssignTargets(s.body, _out);
			}
			else if constexpr (std::is_same_v<T, FunctionDefinition>)
				_yulAssignTargets(s.body, _out);
		}, stmt);
}

// True iff EVERY reference to `_vd` in `_asm` is an assignment TARGET — the
// variable only ever receives a whole aggregate (`result := store`) and is
// never read, indexed, or used as a store address. Such a variable needs no
// pointer model: the assignment is a plain aggregate copy.
static bool _onlyWholeAssignTarget(
	solidity::frontend::InlineAssembly const& _asm,
	solidity::frontend::VariableDeclaration const* _vd)
{
	std::set<solidity::yul::Identifier const*> refs;
	for (auto const& ref: _asm.annotation().externalReferences)
		if (ref.second.declaration == _vd)
			refs.insert(ref.first);
	if (refs.empty())
		return false;
	std::set<solidity::yul::Identifier const*> targets;
	_yulAssignTargets(_asm.operations().root(), targets);
	for (auto const* r: refs)
		if (!targets.count(r))
			return false;
	return true;
}

class AssemblyAggregateScanner: public solidity::frontend::ASTConstVisitor
{
public:
	std::set<int64_t>& ids;
	bool universalMemory;
	explicit AssemblyAggregateScanner(
		std::set<int64_t>& _ids, bool _universalMemory)
		: ids(_ids), universalMemory(_universalMemory)
	{}

	bool visit(solidity::frontend::InlineAssembly const& _asm) override
	{
		for (auto const& ref: _asm.annotation().externalReferences)
		{
			auto const* vd = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				ref.second.declaration);
			if (!vd
				|| vd->referenceLocation()
					!= solidity::frontend::VariableDeclaration::Location::Memory)
				continue;
			auto const* t = vd->type();
			if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(t))
			{
				// Real arrays: always blob-back. bytes/string keep the value model
				// (dedicated tryHandleBytes* handlers preserve x[i]=/x.length/return x,
				// incl. direct `mstore(add(x,32), w)` word writes) EXCEPT a freshly-
				// `new`ed buffer whose pointer ESCAPES into a Yul local — the OZ
				// Strings.toString idiom (`ptr := add(buffer, k)` + `mstore8(ptr,…)`
				// + `return buffer`). Only then do the value handlers lose the writes,
				// so blob-back it (memory-pointer model).
				// A pure PUN TARGET (`address[] memory result; result := store`)
				// only ever RECEIVES an aggregate — never read, indexed, or used
				// as a store address — so it needs no pointer model. Blob-backing
				// it allocated an EMPTY region at the declaration, and since the
				// assignment writes the plain local rather than that region, the
				// later value-use materialised the empty region: OZ
				// `EnumerableSet.values()` returned [] for a non-empty set. The
				// named-return spelling of the same idiom was always fine, which
				// is exactly the inconsistency this removes. Stage 3 keeps the
				// universal pointer model.
				if (!universalMemory && _onlyWholeAssignTarget(_asm, vd))
					continue;
				if (!at->isByteArrayOrString()
					|| universalMemory   // stage 3: universal pointer model
					|| (_isNewAllocatedLocal(vd) && _bufferPointerEscapes(_asm, vd)))
					ids.insert(vd->id());
			}
			else if (dynamic_cast<solidity::frontend::StructType const*>(t))
				ids.insert(vd->id());
		}
		return true;
	}
};

/// Internal/private code outside solc's creation + deployed call graphs is not
/// part of the contract bytecode. Public/external methods are deliberately
/// retained even if an absent/malformed graph ever omits one: the ABI surface
/// is a stronger contract than this optimization.
bool isUnreachableInternalFunction(
	solidity::frontend::FunctionDefinition const& _fn,
	solidity::frontend::ContractDefinition const& _contract,
	ProgramAnalysis const& _analysis)
{
	if (!_analysis.hasContractReachability(_contract.id()))
		return false;
	// LogicSig entry selection happens after contract translation and is not an
	// EVM call-graph root. Keep an explicitly marked entry even when it is
	// internal/private so reachability pruning cannot remove the AVM program.
	for (auto const& modifier: _fn.modifiers())
	{
		auto const& path = modifier->name().path();
		if (!path.empty() && path.back() == "logicsig")
			return false;
	}
	using solidity::frontend::Visibility;
	if (_fn.visibility() != Visibility::Internal
		&& _fn.visibility() != Visibility::Private)
		return false;
	return !_analysis.isFunctionReachable(_contract.id(), _fn.id());
}
} // namespace

std::shared_ptr<awst::Expression> materializeBlobStructValue(
	TypeMapper& _typeMapper,
	solidity::frontend::StructType const* _structType,
	awst::WType const* _wtype,
	std::string const& _offVar,
	awst::SourceLocation const& _loc)
{
	using AB = AssemblyBuilder;
	auto const* structW = dynamic_cast<awst::ARC4Struct const*>(_wtype);
	if (!_structType || !structW)
		return nullptr;
	auto ns = awst::makeNewStruct(structW, _loc);
	unsigned mi = 0;
	for (auto const& m: _structType->structDefinition().members())
	{
		if (!m || !m->type())
			return nullptr;
		if (!m->type()->isValueType())
		{
			Logger::instance().error(
				"--evm-memory-layout: blob-backed struct with non-value member '"
				+ m->name() + "' cannot be used as a value", _loc);
			return nullptr;
		}
		awst::WType const* fieldW = nullptr;
		for (auto const& [fname, ftype]: structW->fields())
			if (fname == m->name()) { fieldW = ftype; break; }
		if (!fieldW)
			return nullptr;
		// one EVM word per field (the layout emitBlobBackValue wrote)
		auto word = AB::readMemWordDirect(
			_typeMapper.profile().scratchLayout,
			awst::makeUInt64BinOp(
				awst::makeVarExpression(_offVar, awst::WType::uint64Type(), _loc),
				awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant(static_cast<uint64_t>(mi * 32), _loc),
				_loc),
			_loc);
		std::shared_ptr<awst::Expression> v;
		if (auto const* un = dynamic_cast<awst::ARC4UIntN const*>(fieldW))
		{
			unsigned nb = static_cast<unsigned>(un->n()) / 8;
			v = awst::makeReinterpretCast(
				awst::makeExtract(std::move(word),
					static_cast<int>(32 - nb), static_cast<int>(nb), _loc),
				fieldW, _loc);
		}
		else if (fieldW == awst::WType::arc4BoolType())
			v = awst::makeARC4Encode(
				awst::makeNumericCompare(
					awst::makeAsBiguint(std::move(word), _loc),
					awst::NumericComparison::Ne,
					awst::makeIntegerConstant("0", _loc, awst::WType::biguintType()),
					_loc), fieldW, _loc);
		else if (fieldW == awst::WType::boolType())
			v = awst::makeNumericCompare(
				awst::makeAsBiguint(std::move(word), _loc),
				awst::NumericComparison::Ne,
				awst::makeIntegerConstant("0", _loc, awst::WType::biguintType()), _loc);
		else if (fieldW == awst::WType::accountType())
			v = awst::makeAsAccount(std::move(word), _loc);
		else if (fieldW == awst::WType::biguintType())
			v = awst::makeAsBiguint(std::move(word), _loc);
		else if (fieldW == awst::WType::uint64Type())
			v = awst::makeBtoi(awst::makeExtract(std::move(word), 24, 8, _loc), _loc);
		else if (auto const* bw = dynamic_cast<awst::BytesWType const*>(fieldW);
			bw && bw->length().has_value())
			v = awst::makeReinterpretCast(
				awst::makeExtract(std::move(word),
					static_cast<int>(32 - *bw->length()),
					static_cast<int>(*bw->length()), _loc), fieldW, _loc);
		else
		{
			Logger::instance().error(
				"--evm-memory-layout: cannot materialise blob struct member '"
				+ m->name() + "' of type '" + std::string(fieldW->name()) + "'",
				_loc);
			return nullptr;
		}
		ns->values[m->name()] = std::move(v);
		mi++;
	}
	return ns;
}

void emitAsmParamSpills(
	TypeMapper& _typeMapper,
	sol_ast::FunctionContext& _fn,
	solidity::frontend::Block const& _block,
	std::string const& _sourceFile,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (!_typeMapper.profile().evmMemoryLayout)
		return;
	// collect the DECLS asm references (the aggregate scanner only keeps ids)
	struct DeclScan: solidity::frontend::ASTConstVisitor
	{
		std::map<int64_t, solidity::frontend::VariableDeclaration const*> decls;
		bool visit(solidity::frontend::InlineAssembly const& _asm) override
		{
			for (auto const& ref: _asm.annotation().externalReferences)
				if (auto const* vd = dynamic_cast<
						solidity::frontend::VariableDeclaration const*>(
						ref.second.declaration))
					decls[vd->id()] = vd;
			return true;
		}
	} scan;
	_block.accept(scan);
	for (auto const& [id, vd]: scan.decls)
	{
		if (!vd->isCallableOrCatchParameter()
			|| vd->referenceLocation()
				!= solidity::frontend::VariableDeclaration::Location::Memory
			|| vd->name().empty())
			continue;
		auto const* t = vd->type();
		bool aggregate = dynamic_cast<solidity::frontend::ArrayType const*>(t)
			|| dynamic_cast<solidity::frontend::StructType const*>(t);
		if (!aggregate)
			continue;
		if (!_fn.findBlobAggregate(id).empty())
			continue;   // already pointer-modeled (>4KB path)
		auto const* wt = _typeMapper.map(t);
		std::string offN = "__blobagg_off_" + std::to_string(id);
		awst::SourceLocation loc0 = makeLoc(_typeMapper, _sourceFile, vd->location());
		if (emitBlobBackValue(_typeMapper, t, wt,
				awst::makeVarExpression(vd->name(), wt, loc0),
				offN, static_cast<int>(id), loc0, _out))
			_fn.setBlobAggregate(id, offN);
	}
}

bool blockUsesDeclInAsm(
	solidity::frontend::Block const& _block, int64_t _declId)
{
	struct Scan: solidity::frontend::ASTConstVisitor
	{
		int64_t id;
		bool found = false;
		bool visit(solidity::frontend::InlineAssembly const& _asm) override
		{
			for (auto const& ref: _asm.annotation().externalReferences)
				if (ref.second.declaration && ref.second.declaration->id() == id)
					found = true;
			return !found;
		}
	} scan;
	scan.id = _declId;
	_block.accept(scan);
	return scan.found;
}

bool emitBlobBackValue(
	TypeMapper& typeMapper,
	solidity::frontend::Type const* declType,
	awst::WType const* wtype,
	std::shared_ptr<awst::Expression> value,
	std::string const& offVar,
	int uniqueId,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out)
{
	using AB = AssemblyBuilder;
	using namespace solidity::frontend;
	auto const* at2 = dynamic_cast<ArrayType const*>(declType);
	auto const* st2 = dynamic_cast<StructType const*>(declType);
	std::string vn = "__blobinit_" + std::to_string(uniqueId);
	if (at2 && at2->isByteArrayOrString())
	{
		out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(vn, awst::WType::bytesType(), loc),
			awst::makeAsBytes(std::move(value), loc), loc));
		auto vRef = [&]() { return awst::makeVarExpression(
			vn, awst::WType::bytesType(), loc); };
		for (auto& s2: AB::emitBytesBlobAlloc(typeMapper.profile().scratchLayout,
				awst::makeLen(vRef(), loc), offVar, uniqueId, loc))
			out.push_back(std::move(s2));
		AB::writeMemBytesDirect(typeMapper.profile().scratchLayout,
			awst::makeUInt64BinOp(
				awst::makeVarExpression(offVar, awst::WType::uint64Type(), loc),
				awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant("32", loc), loc),
			vRef(), uniqueId, loc, out);
		return true;
	}
	if (st2)
	{
		auto const* structW = dynamic_cast<awst::ARC4Struct const*>(wtype);
		out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(vn, wtype, loc), std::move(value), loc));
		auto vRef = [&]() { return awst::makeVarExpression(vn, wtype, loc); };
		auto const& members = st2->structDefinition().members();
		int sz2 = static_cast<int>(members.size()) * 32;
		out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(offVar, awst::WType::uint64Type(), loc),
			awst::makeExtractUInt64(
				awst::makeLoadSlot(typeMapper.profile().scratchLayout.memoryFirst(), loc),
				awst::makeIntegerConstant("88", loc), loc), loc));
		for (auto& s2: AB::emitFreeMemoryBump(
				typeMapper.profile().scratchLayout, sz2, loc, uniqueId))
			out.push_back(std::move(s2));
		unsigned mi = 0;
		for (auto const& m2: members)
		{
			if (!m2 || !m2->type() || !m2->type()->isValueType())
			{
				Logger::instance().error(
					"--evm-memory-layout: blob-backing struct with non-value "
					"member '" + (m2 ? m2->name() : "?") + "' is not yet "
					"supported", loc);
				return false;
			}
			awst::WType const* fieldW = nullptr;
			if (structW)
				for (auto const& [fname, ftype]: structW->fields())
					if (fname == m2->name()) { fieldW = ftype; break; }
			auto field = awst::makeFieldExpression(vRef(), m2->name(),
				fieldW ? fieldW : typeMapper.map(m2->type()), loc);
			std::shared_ptr<awst::Expression> w32;
			if (field->wtype == awst::WType::arc4BoolType())
			{
				auto b2 = awst::makeARC4Decode(std::move(field),
					awst::WType::boolType(), loc);
				auto u2 = awst::makeConditional(std::move(b2),
					awst::makeIntegerConstant("1", loc),
					awst::makeIntegerConstant("0", loc),
					awst::WType::uint64Type(), loc);
				w32 = awst::makeLeftPadToN(awst::makeItob(std::move(u2), loc), 32, loc);
			}
			else
				w32 = awst::makeLeftPadToN(
					awst::makeAsBytes(std::move(field), loc), 32, loc);
			std::vector<std::shared_ptr<awst::Statement>> ws2;
			AB::writeMemWordDirect(typeMapper.profile().scratchLayout,
				awst::makeUInt64BinOp(
					awst::makeVarExpression(offVar, awst::WType::uint64Type(), loc),
					awst::UInt64BinaryOperator::Add,
					awst::makeIntegerConstant(static_cast<uint64_t>(mi * 32), loc),
					loc),
				std::move(w32), loc, ws2);
			for (auto& st3: ws2)
				out.push_back(std::move(st3));
			mi++;
		}
		return true;
	}
	if (at2 && computeEncodedElementSize(
			typeMapper.mapSolTypeToARC4(at2->baseType())) == 32)
	{
		out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(vn, awst::WType::bytesType(), loc),
			awst::makeAsBytes(std::move(value), loc), loc));
		auto vRef = [&]() { return awst::makeVarExpression(
			vn, awst::WType::bytesType(), loc); };
		if (at2->isDynamicallySized())
		{
			auto count = awst::makeExtractUInt16(vRef(),
				awst::makeIntegerConstant("0", loc), loc);
			for (auto& s2: AB::emitBytesBlobAlloc(typeMapper.profile().scratchLayout,
					awst::makeUInt64BinOp(std::move(count),
						awst::UInt64BinaryOperator::Mult,
						awst::makeIntegerConstant("32", loc), loc),
					offVar, uniqueId, loc))
				out.push_back(std::move(s2));
			std::vector<std::shared_ptr<awst::Statement>> ws2;
			AB::writeMemWordDirect(typeMapper.profile().scratchLayout,
				awst::makeVarExpression(offVar, awst::WType::uint64Type(), loc),
				awst::makeLeftPadToN(awst::makeItob(
					awst::makeExtractUInt16(vRef(),
						awst::makeIntegerConstant("0", loc), loc), loc), 32, loc),
				loc, ws2);
			for (auto& st3: ws2)
				out.push_back(std::move(st3));
			AB::writeMemBytesDirect(typeMapper.profile().scratchLayout,
				awst::makeUInt64BinOp(
					awst::makeVarExpression(offVar, awst::WType::uint64Type(), loc),
					awst::UInt64BinaryOperator::Add,
					awst::makeIntegerConstant("32", loc), loc),
				awst::makeExtract(vRef(), 2, 0, loc), uniqueId, loc, out);
		}
		else
		{
			out.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(offVar, awst::WType::uint64Type(), loc),
				awst::makeExtractUInt64(
					awst::makeLoadSlot(typeMapper.profile().scratchLayout.memoryFirst(), loc),
					awst::makeIntegerConstant("88", loc), loc), loc));
			int sz3 = computeEncodedElementSize(wtype);
			for (auto& s2: AB::emitFreeMemoryBump(typeMapper.profile().scratchLayout,
					sz3 > 0 ? sz3 : 32, loc, uniqueId))
				out.push_back(std::move(s2));
			AB::writeMemBytesDirect(typeMapper.profile().scratchLayout,
				awst::makeVarExpression(offVar, awst::WType::uint64Type(), loc),
				vRef(), uniqueId, loc, out);
		}
		return true;
	}
	Logger::instance().error(
		"--evm-memory-layout: cannot blob-back this value shape", loc);
	return false;
}

void markAssemblyAggregates(
	sol_ast::FunctionContext& _fn,
	solidity::frontend::Block const& _block)
{
	std::set<int64_t> asmAggIds;
	AssemblyAggregateScanner scanner{
		asmAggIds, _fn.tr.typeMapper.profile().evmMemoryLayout};
	_block.accept(scanner);
	for (int64_t id: asmAggIds)
		_fn.markAssemblyAggregate(id);
}

std::shared_ptr<awst::Block> buildBlock(
	sol_ast::FunctionContext& _ctx,
	solidity::frontend::Block const& _block,
	std::shared_ptr<awst::Block> _placeholder)
{
	auto& fn = _ctx;
	auto& exprBuilder = _ctx.tr.contractCtx;
	auto& typeMapper = _ctx.tr.typeMapper;
	auto const& sourceFile = _ctx.tr.sourceFile;
	auto fnGuard = exprBuilder.pushScopeRaii(&fn);
	auto blk = _placeholder
		? sol_ast::BlockContext::top(fn).withPlaceholder(_placeholder)
		: sol_ast::BlockContext::top(fn);
	auto blkGuard = exprBuilder.pushScopeRaii(&blk);

	// Mapping storage-ref params: `m[k]` resolves the dynamic box-key prefix at runtime.
	for (auto const* mp: _ctx.mappingKeyParams)
		if (mp && !mp->name().empty())
			fn.setMappingKeyParam(mp->id(), mp->name());

	// --evm-storage-layout: storage-ref params / named storage returns are
	// biguint slot handles — register so slot-handle machinery resolves them.
	for (auto const* sp: _ctx.slotRefParams)
		if (sp && !sp->name().empty())
			fn.setSlotStorageRef(sp->id(), awst::makeVarExpression(
				sp->name(), awst::WType::biguintType(), awst::SourceLocation{}));

	// Offset-convention struct-ref params (handle-model dual handle): register the companion
	// uint64 offset var so the body's `s.field` writes hit the element slice via
	// box_replace(key, offset+fieldOff). The offset param itself is in the subroutine signature
	// (FunctionBuilder) and supplied by the caller (SolInternalCall).
	for (auto const* mp: _ctx.mappingKeyParams)
		if (mp && !mp->name().empty()
			&& typeMapper.analysis().structRefOffsetParams.count(mp->id()))
			fn.setStructRefOffset(mp->id(), mp->name() + "__off");

	// Named returns >4 KB: blob-backed aggregates (pointer model) so `p.field[i]`
	// lowers to multi-slot blob word access. Base offset assigned + FMP bumped in
	// FunctionBuilder.
	for (auto const* rp: _ctx.namedReturns)
	{
		if (!rp || rp->name().empty()
			|| rp->referenceLocation() != solidity::frontend::VariableDeclaration::Location::Memory)
			continue;
		auto const* rpType = typeMapper.map(rp->type());
		if (memoryUsesBlob(rpType))
			fn.setBlobAggregate(rp->id(), "__blobagg_off_" + std::to_string(rp->id()));
	}

	// Blob-agg params >4 KB: param's local IS the uint64 base offset (caller passed
	// it — see SolInternalCall/SolIdentifier); no FMP bump needed.
	for (auto const* p: _ctx.blobAggParams)
		if (p && !p->name().empty())
			fn.setBlobAggregate(p->id(), p->name());

	// Promote memory aggregates used as values in inline assembly to blob-backed
	// (Yul memory pointer). Must mark before body translation so SolVariableDeclaration
	// blob-backs them at their declaration. Not run during modifier re-entrancy
	// (_placeholder set) — pre-built placeholder contexts are unsafe to re-walk.
	if (!_placeholder)
		markAssemblyAggregates(fn, _block);

	// --evm-memory-layout: MEMORY PARAMS the assembly treats as pointers
	// (`keccak256(s, 32)` on a `string memory s` param) — spill the incoming
	// VALUE into a blob region at function entry and register the param as
	// blob-backed, so asm gets a real offset and value uses read it back.
	std::vector<std::shared_ptr<awst::Statement>> paramSpills;
	if (!_placeholder)
		emitAsmParamSpills(typeMapper, fn, _block, sourceFile, paramSpills);

	auto body = sol_ast::buildBlock(blk, _block);
	if (!paramSpills.empty())
		body->body.insert(body->body.begin(),
			std::make_move_iterator(paramSpills.begin()),
			std::make_move_iterator(paramSpills.end()));
	return body;
}

// ContractBuilder wrappers — route through free-function API.

awst::SourceLocation ContractBuilder::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	return ::puyasol::builder::makeLoc(m_typeMapper, m_sourceFile, _solLoc);
}

std::shared_ptr<awst::Block> ContractBuilder::buildBlock(
	solidity::frontend::Block const& _block)
{
	auto& ctx = m_functionCtx.value();
	return ::puyasol::builder::buildBlock(ctx, _block, ctx.placeholder);
}

void ContractBuilder::setFunctionContext(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	awst::WType const* _returnType,
	std::map<std::string, unsigned> const& _bitWidths,
	std::map<std::string, solidity::frontend::Type const*> const& _paramSolTypes)
{
	auto& ctx = m_functionCtx.value();
	ctx.params = _params;
	ctx.returnType = _returnType;
	ctx.paramBitWidths = _bitWidths;
	ctx.paramSolTypes = _paramSolTypes;
	ctx.namedReturns.clear();
	ctx.mappingKeyParams.clear();
	ctx.boxKeyStructParams.clear();
	ctx.blobAggParams.clear();
	ctx.placeholder.reset();
	ctx.inConstructor = false;
	ctx.frameIsProgram = false;
	ctx.encodeReturnsAtBuildTime = false;
	ctx.returnAsmWrap = false;
	ctx.returnWirePlan.clear();
	ctx.seededCalldataPointers.clear();
	ctx.slotRefParams.clear();
}

void ContractBuilder::setPlaceholderBody(std::shared_ptr<awst::Block> _body)
{
	m_functionCtx->placeholder = std::move(_body);
}

void ContractBuilder::prependNonPayableCheck(awst::ContractMethod& _method,
	std::string const& _arc4Selector)
{
	// Only ARC4-dispatched methods are externally callable.
	if (!_method.arc4MethodConfig.has_value())
		return;
	if (!_method.body)
		return;

	auto loc = _method.sourceLocation;

	auto groupIdx = awst::makeTxn(std::string("GroupIndex"), awst::WType::uint64Type(), loc);

	auto hasPayment = awst::makeNumericCompare(
		groupIdx, awst::NumericComparison::Gt,
		awst::makeIntegerConstant("0", loc), loc);

	auto groupIdx2 = awst::makeTxn(std::string("GroupIndex"), awst::WType::uint64Type(), loc);
	auto payIdx = awst::makeUInt64BinOp(
		std::move(groupIdx2), awst::UInt64BinaryOperator::Sub,
		awst::makeIntegerConstant("1", loc), loc);

	auto amount = awst::makeGtxns(
		"Amount", std::move(payIdx), awst::WType::uint64Type(), loc);

	// Mirrors msg.value shape — avoids GroupIndex-1 when GroupIndex==0 (underflow-safe).
	auto msgValue = awst::makeConditional(
		std::move(hasPayment), std::move(amount),
		awst::makeIntegerConstant("0", loc),
		awst::WType::uint64Type(), loc);

	auto isZero = awst::makeNumericCompare(
		std::move(msgValue), awst::NumericComparison::Eq,
		awst::makeIntegerConstant("0", loc), loc);

	auto assertStmt = awst::makeExpressionStatement(
		awst::makeAssert(std::move(isZero), loc, "not payable"), loc);

	// Gate on the ROUTER having dispatched THIS method. The guard reads a
	// TRANSACTION-level fact (the preceding payment), but it lives in the
	// method BODY — which an internal `callsub` from another method shares. So
	// a PAYABLE function that internally calls a non-payable public one
	// re-evaluated this against the same group and reverted on its own,
	// legitimate payment: friend.tech's payable `buyShares` calls
	// `getPrice(uint256,uint256)`, and every buy with value died on
	// `assert // not payable` inside getPrice. Extremely common shape
	// (buy/sell calling a public price view), invisible until msg.value
	// actually started flowing.
	//
	// ApplicationArgs[0] carries the dispatched method's selector, so it tells
	// entry-from-router apart from entry-from-callsub. Without a selector to
	// compare (empty), keep the unconditional guard — same behaviour as before.
	if (!_arc4Selector.empty())
	{
		auto numArgs = awst::makeTxn(
			std::string("NumAppArgs"), awst::WType::uint64Type(), loc);
		auto hasArgs = awst::makeNumericCompare(
			std::move(numArgs), awst::NumericComparison::Gt,
			awst::makeIntegerConstant("0", loc), loc);
		auto selMatches = awst::makeBytesComparison(
			awst::makeAppArg(0, loc),
			awst::EqualityComparison::Eq,
			awst::makeMethodConstant(_arc4Selector, awst::WType::bytesType(), loc),
			loc);
		auto dispatched = awst::makeBoolBinOp(
			std::move(hasArgs), awst::BinaryBooleanOperator::And,
			std::move(selMatches), loc);
		auto thenBlock = awst::makeBlock(loc);
		thenBlock->body.push_back(std::move(assertStmt));
		_method.body->body.insert(
			_method.body->body.begin(),
			awst::makeIfElse(std::move(dispatched), std::move(thenBlock), nullptr, loc));
		return;
	}
	_method.body->body.insert(_method.body->body.begin(), std::move(assertStmt));
}

std::shared_ptr<awst::Contract> ContractBuilder::build(
	solidity::frontend::ContractDefinition const& _contract,
	StorageRuntimePlan const& _storagePlan,
	bool _emitEvmStorageRuntime
)
{
	m_currentContract = &_contract;
	std::string contractName = _contract.name();
	std::string contractId = m_sourceFile + "." + contractName;

	// Reset the generated-name counters: a contract's temp/subroutine names
	// (`__mod_retval_N`, `f__mod0_N`, …) must depend only on its own content,
	// not on how many contracts compiled before it in the batch (deterministic
	// multi-contract output; prerequisite for parallel per-contract compiles).
	awst::NameGen::resetAll();

	// Reset Yul subroutine sink (drained below).
	m_typeMapper.artifacts().pendingYulSubroutines.clear();

	// Collect transient state variables
	m_transientStorage.collectVars(_contract, m_typeMapper);
	// Note: setTransientStorage called after m_exprBuilder is created (below)

	// Overloaded names: true overloads (same name, different params) only;
	// virtual overrides occupy the same slot and don't count.
	// Must be computed before translator creation so ctor uses correct names.
	m_overloadedNames.clear();
	// Function ids that a more-derived contract overrides — computed here for
	// overload naming, reused below to skip re-emitting overridden inherited
	// functions.
	std::set<int64_t> overriddenIds;
	{
		forEachDefinedFunction(_contract, [&](auto const* func)
		{
			if (func->isConstructor() || !func->isImplemented())
				return;
			// Mark all base functions of this override as overridden
			for (auto const* baseFunc: func->annotation().baseFunctions)
				overriddenIds.insert(baseFunc->id());
		});

		std::unordered_map<std::string, int> nameCount;
		forEachDefinedFunction(_contract, [&](auto const* func)
		{
			if (func->isConstructor() || !func->isImplemented())
				return;
			// Skip functions that have been overridden by a more-derived version
			if (overriddenIds.count(func->id()))
				return;
			nameCount[func->name()]++;
		});
		for (auto const& [name, count]: nameCount)
		{
			if (count > 1)
			{
				m_overloadedNames.insert(name);
				Logger::instance().debug("Overloaded function: " + name + " (" + std::to_string(count) + " versions)");
			}
		}
	}

	m_exprBuilder = std::make_unique<eb::ContractContext>(
		m_typeMapper, m_storageMapper, m_sourceFile, contractName,
		m_overloadedNames, m_functionSymbols,
		m_functionPointers
	);
	m_exprBuilder->currentContract = &_contract;
	m_exprBuilder->viaIRSequencing = m_viaIR;

	// One session-owned layout feeds state access, inline-assembly slot routing,
	// and runtime-dispatch generation. In EVM mode it is solc's exact linearized
	// layout; default mode uses the compiler's physical AVM layout.
	m_exprBuilder->storageLayout = &_storagePlan.dispatchLayout;

	// Pre-populate host-bound function map before translation so the call
	// resolver routes them as InstanceMethodTargets.
	for (auto const* function: m_hostBoundFunctions)
	{
		if (!function) continue;
		auto const* scope = function->annotation().contract;
		std::string methodName = "__hostfn_"
			+ (scope ? scope->name() : std::string("free"))
			+ "_" + function->name() + "_" + std::to_string(function->id());
		m_exprBuilder->internalizedFunctionNames[function->id()] = methodName;
	}

	// In-place emplace — TranslationContext caches a pointer to its own scopeState_;
	// copy/move construction would dangle that pointer.
	m_tr.emplace(*m_exprBuilder, m_typeMapper, m_sourceFile);
	m_exprBuilder->currentScope = &*m_tr;
	m_functionCtx.emplace(*m_tr,
		std::vector<std::pair<std::string, awst::WType const*>>{},
		nullptr, std::map<std::string, unsigned>{});
	m_functionCtx->currentContract = &_contract;

	m_exprBuilder->transientStorage =
		m_transientStorage.hasTransientVars() ? &m_transientStorage : nullptr;
	// StorageBackend is per-contract (TransientStorage is per-contract).
	m_storageBackend.emplace(m_storageMapper, m_exprBuilder->transientStorage);
	m_exprBuilder->storageBackend = &*m_storageBackend;

	eb::FunctionPointerBuilder::setCurrentCref(*m_exprBuilder, contractId);

	auto contract = std::make_shared<awst::Contract>();
	contract->sourceLocation = makeLoc(_contract.location());
	contract->id = contractId;
	contract->name = contractName;

	if (_contract.documentation())
	{
		std::string const& doc = *_contract.documentation()->text();
		contract->description = doc;
		// uros splitter opt-in: `@custom:splitter <selector>` (e.g. "uros").
		contract->splitter = natSpecTagValue(doc, "custom:splitter");
	}

	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base != &_contract)
			contract->methodResolutionOrder.push_back(
				m_sourceFile + "." + base->name()
			);
	}

	// --evm-storage-layout: state lives in opaque numbered slots — no per-var
	// ARC-56 declarations (the reason the mode is opt-in; see the design doc).
	if (!m_typeMapper.profile().evmStorageLayout)
		contract->appState = m_storageMapper.mapStateVariables(_contract, m_sourceFile);

	// EVM-memory scratch slots (default 0-4; raisable via
	// --evm-memory-slots) plus transient + flash-accounting slots.
	contract->reservedScratchSpace = m_typeMapper.profile().scratchLayout.reservedSlots();

	collectSuperCallMetadata(_contract);

	// Snapshot super targets so the ctor body (translated in buildApprovalProgram)
	// can resolve super.f() to f__super_N rather than the contract's own f.
	m_allSuperTargetNames = m_tr->allSuperTargets();

	// Approval and clear programs
	m_postInitMethod.reset();
	contract->approvalProgram = buildApprovalProgram(_contract, contractName);
	contract->clearProgram = buildClearProgram(_contract, contractName);

	if (m_postInitMethod)
	{
		awst::AppStorageDefinition ctorPendingState;
		ctorPendingState.memberName = "__ctor_pending";
		ctorPendingState.sourceLocation = contract->approvalProgram.sourceLocation;
		ctorPendingState.storageKind = awst::AppStorageKind::AppGlobal;
		ctorPendingState.storageWType = awst::WType::uint64Type();
		ctorPendingState.key = awst::makeUtf8BytesConstant(
			"__ctor_pending", ctorPendingState.sourceLocation);
		contract->appState.push_back(std::move(ctorPendingState));

		contract->methods.push_back(std::move(*m_postInitMethod));
		m_postInitMethod.reset();
	}


	std::set<std::string> translatedFunctions;
	for (auto const* func: _contract.definedFunctions())
	{
		if (func->isConstructor())
			continue;

		if (isUnreachableInternalFunction(
				*func, _contract, m_typeMapper.analysis()))
		{
			Logger::instance().debug(
				"skipping unreachable internal/private function `"
				+ func->name() + "` (absent from solc's call graphs)",
				makeLoc(func->location()));
			continue;
		}
		std::string key = func->name();
		if (m_overloadedNames.count(key))
			key += "#" + std::to_string(func->id());
		translatedFunctions.insert(key);
		clearSuperOverrides();
		applySuperOverridesFor(func->id());
		// fallback/receive have empty Solidity names; give explicit memberName.
		std::string nameOverride;
		if (func->isFallback())
			nameOverride = "__fallback";
		else if (func->isReceive())
			nameOverride = "__receive";
		auto method = buildFunction(*func, contractName, nameOverride);
		contract->methods.push_back(std::move(method));
		for (auto& sub: m_modifierSubroutines)
			contract->methods.push_back(std::move(sub));
		m_modifierSubroutines.clear();
	}

	// Getters before inherited functions so `uint256 public override test` beats
	// an inherited `function test()`.
	buildPublicStateVariableGetters(_contract, *contract, contractName, translatedFunctions);

	// Inherited functions (after getters — same precedence rule).
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base == &_contract)
			continue; // Already handled above

		for (auto const* func: base->definedFunctions())
		{
			if (func->isConstructor())
				continue;

			// A base function overridden by a more-derived version must NOT be
			// re-emitted: the derived override already occupies the same ABI
			// route. The name#id dedup key alone let it through (different id),
			// producing a duplicate ABI method (stale base body) that routed
			// on the same selector — safe only by MRO emission order.
			if (overriddenIds.count(func->id()))
				continue;

			std::string key = func->name();
			if (m_overloadedNames.count(key))
				key += "#" + std::to_string(func->id());
			if (translatedFunctions.count(key))
				continue;

			if (!func->isImplemented())
				continue;
			if (isUnreachableInternalFunction(
					*func, _contract, m_typeMapper.analysis()))
				continue;

			translatedFunctions.insert(key);
			// Set up MRO-correct super targets for this inherited function
			clearSuperOverrides();
			applySuperOverridesFor(func->id());
			std::string nameOverride2;
			if (func->isFallback())
				nameOverride2 = "__fallback";
			else if (func->isReceive())
				nameOverride2 = "__receive";
			auto method = buildFunction(*func, contractName, nameOverride2);
			contract->methods.push_back(std::move(method));
			for (auto& sub: m_modifierSubroutines)
				contract->methods.push_back(std::move(sub));
			m_modifierSubroutines.clear();
		}
	}

	// Emit MRO / fallback / explicit-base super subroutines now that all
	// regular method bodies are translated.
	emitSuperSubroutines(*contract, contractName);

	// Emit functions whose lowering requires a concrete contract host. This
	// includes function-pointer dispatch and default-layout storage assembly.
	for (auto const* function: m_hostBoundFunctions)
	{
		if (!function || !function->isImplemented()) continue;
		auto nameIt = m_exprBuilder->internalizedFunctionNames.find(function->id());
		if (nameIt == m_exprBuilder->internalizedFunctionNames.end()) continue;
		clearSuperOverrides();
		auto method = buildFunction(
			*function, contractName, nameIt->second, /*asInternalCopy=*/true);
		contract->methods.push_back(std::move(method));
		for (auto& sub: m_modifierSubroutines)
			contract->methods.push_back(std::move(sub));
		m_modifierSubroutines.clear();
	}

	// Generate __storage_read/__storage_write dispatch subroutines
	// for assembly sload/sstore support
	// EVM-layout runtime helpers have unit-global SubroutineIDs and bodies that
	// are specialized from unit-global profile flags. Emit them once for the
	// whole unit; generating a copy per concrete contract inflated multi-contract
	// AWST by hundreds of kilobytes and made duplicate-ID resolution ambiguous.
	// Default-layout dispatch remains contract-specific and is always emitted.
	if (_emitEvmStorageRuntime
		|| (!m_typeMapper.profile().evmStorageLayout && _storagePlan.needsDispatch()))
		buildStorageDispatch(_contract, _storagePlan, contract.get(), contractName);

	// Generate function pointer dispatch tables
	{
		// Set subroutine IDs for library/free function targets so dispatch
		// uses SubroutineID (resolvable by puya) instead of InstanceMethodTarget.
		eb::FunctionPointerBuilder::setSubroutineIds(
			*m_exprBuilder, m_functionSymbols);

		std::string cref = m_sourceFile + "." + contractName;
		awst::SourceLocation loc;
		loc.file = m_sourceFile;
		auto& dispCtx = *m_exprBuilder;
		auto dispatchMethods = eb::FunctionPointerBuilder::generateDispatchMethods(
			dispCtx, cref, loc, &m_dispatchSubroutines);
		for (auto& m : dispatchMethods)
			contract->methods.push_back(std::move(m));
		eb::FunctionPointerBuilder::reset(*m_exprBuilder);
	}

	// Drain any Subroutines emitted for recursive Yul functions so the
	// contract-builder caller picks them up alongside fn-ptr dispatchers.
	{
		auto yulSubs = std::move(m_typeMapper.artifacts().pendingYulSubroutines);
		m_typeMapper.artifacts().pendingYulSubroutines.clear();
		for (auto& sub: yulSubs)
			m_dispatchSubroutines.push_back(std::move(sub));
	}

	// uros splitter: the backend requires EVERY ABI method to declare a chunk
	// when the contract opts in. User methods get theirs from @custom:uros-chunk,
	// but compiler-synthesized ABI methods (public-state-var getters, __postInit,
	// __fallback, __receive) have none. Assign any still-unchunked ABI method to
	// a default "shell" chunk so the backend can place them. No effect unless the
	// contract set @custom:splitter, so non-split contracts are unchanged.
	if (!contract->splitter.empty())
	{
		for (auto& m: contract->methods)
		{
			if (!m.arc4MethodConfig.has_value())
				continue;
			if (auto* abi = std::get_if<awst::ARC4ABIMethodConfig>(&*m.arc4MethodConfig))
			{
				if (abi->chunk.empty())
					abi->chunk = "shell";
			}
		}
	}

	// Default-layout dispatch bodies are contract-specific because they route
	// logical slots to this contract's named AVM cells. Scope every generated
	// call to the same contract-specific root ID. EVM-layout runtime helpers are
	// compilation-unit singletons and retain their stable global IDs.
	if (!m_typeMapper.profile().evmStorageLayout && _storagePlan.needsDispatch())
	{
		auto const scopeStorageCall = [&](awst::Expression& expression) {
			auto* call = dynamic_cast<awst::SubroutineCallExpression*>(&expression);
			if (!call)
				return;
			auto* id = std::get_if<awst::SubroutineID>(&call->target);
			if (!id)
				return;
			if (id->target == "__puyasol___storage_read")
				id->target = contractId + ".__storage_read";
			else if (id->target == "__puyasol___storage_write")
				id->target = contractId + ".__storage_write";
		};
		awst::visitExpressions(contract->approvalProgram, scopeStorageCall);
		awst::visitExpressions(contract->clearProgram, scopeStorageCall);
		for (auto& method: contract->methods)
			awst::visitExpressions(method, scopeStorageCall);
		for (auto& subroutine: m_dispatchSubroutines)
			if (subroutine && subroutine->body)
				awst::visitExpressions(*subroutine->body, scopeStorageCall);
	}

	return contract;
}



} // namespace puyasol::builder
