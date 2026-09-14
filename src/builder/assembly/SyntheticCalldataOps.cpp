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
#include "builder/AwstShorthand.h"
#include "awst/NameGen.h"
#include "builder/abi/EvmAbiEncode.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>
#include <libsolidity/ast/TypeProvider.h>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>
#include <libyul/optimiser/ASTWalker.h>

namespace puyasol::builder
{

bool AssemblyBuilder::detectDynamicCalldataAccess(solidity::yul::Block const& _block)
{
	using namespace solidity::yul;
	struct Query: ASTWalker
	{
		using ASTWalker::operator();
		std::function<bool(Identifier const&)> pointer;
		std::function<bool(FunctionCall const&)> calldata;
		bool found = false;
		void operator()(Identifier const& id) override { found = found || pointer(id); }
		void operator()(FunctionCall const& call) override
		{
			found = found || calldata(call);
			if (!found) ASTWalker::operator()(call);
		}
		void operator()(Block const& block) override { if (!found) ASTWalker::operator()(block); }
	} query;
	// ASTWalker visits assignment targets as well as reads. Pointer-only
	// writes therefore seed exactly the same calldata blob as pointer reads.
	query.pointer = [&](Identifier const& id) {
		std::string name = resolveVarRef(id);
		if (m_frame.calldataStaticPtrNames.count(name)) return true;
		auto dot = name.rfind('.');
		if (dot == std::string::npos) return false;
		auto suffix = name.substr(dot + 1);
		if (suffix != "offset" && suffix != "length") return false;
		auto base = name.substr(0, dot);
		auto it = m_frame.locals.find(base);
		return (it != m_frame.locals.end() && isDynamicCalldataType(it->second))
			|| m_frame.calldataPointerNames.count(base);
	};
	query.calldata = [&](FunctionCall const& call) {
		auto name = getFunctionName(call.functionName);
		if (name == "calldatasize" || (name == "calldatacopy" && call.arguments.size() == 3))
			return true;
		if (name != "calldataload" || call.arguments.size() != 1) return false;
		auto offset = resolveConstantYulValue(call.arguments[0]);
		if (!offset) return true;
		auto it = m_frame.calldataMap.find(*offset);
		if (it == m_frame.calldataMap.end()) return true;
		auto const* type = calldataSolType(it->second.paramName);
		return solTypeUsable(type) && type->isDynamicallyEncoded();
	};
	query(_block);
	return query.found;
}

namespace
{

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
	if (_solLeaf)
		return codec::valueToEvmWord(m_typeMapper, _solLeaf, std::move(_value), _loc);
	// Genuine synthetic words have no Solidity declaration. Their physical
	// numeric/boolean carrier is explicit; never infer signedness from it.
	if (_value->wtype == awst::WType::arc4BoolType())
		_value = awst::makeARC4Decode(std::move(_value), awst::WType::boolType(), _loc);
	if (_value->wtype == awst::WType::boolType())
		_value = awst::makeAsUInt64(std::move(_value), _loc);
	return padTo32Bytes(std::move(_value), _loc);
}

bool AssemblyBuilder::leafNeedsEvmWord(solidity::frontend::Type const* _solLeaf)
{
	using namespace solidity::frontend;
	_solLeaf = codec::underlyingType(_solLeaf);
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
	auto u64 = [&](uint64_t v) { return shorthand::u64(v, _loc); };
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
	auto u64 = [&](uint64_t v) { return shorthand::u64(v, _loc); };
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
	for (auto const& [name, type]: m_context->calldataParams)
	{
		auto const* solType = calldataSolType(name);
		bool const dynamicallyEncoded = solTypeUsable(solType)
			? solType->isDynamicallyEncoded()
			: isDynamicCalldataType(type);
		// STATIC calldata pointer param (struct / fixed array) referenced as a bare
		// pointer in this block: seed __cd_off_<name> with its constant data offset
		// (statics live inline in the head area — m_frame.localConstants holds the byte pos).
		if (!dynamicallyEncoded)
		{
			if (!m_frame.calldataStaticPtrNames.count(name)) continue;
			auto cdIt = m_frame.localConstants.find(name);
			if (cdIt == m_frame.localConstants.end()) continue;
			if (m_frame.seededCalldataPointers)
			{
				if (m_frame.seededCalldataPointers->count(name)) continue;
				m_frame.seededCalldataPointers->insert(name);
			}
			_out.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression("__cd_off_" + name, awst::WType::biguintType(), _loc),
				awst::makeIntegerConstant(cdIt->second, _loc, awst::WType::biguintType()), _loc));
			continue;
		}
		auto cdIt = m_frame.localConstants.find(name);
		if (cdIt == m_frame.localConstants.end()) continue;
		// Seed ONCE per function: a later block must see a pointer mutated by an
		// earlier block (x.offset := V), not a fresh canonical re-seed.
		if (m_frame.seededCalldataPointers)
		{
			if (m_frame.seededCalldataPointers->count(name)) continue;
			m_frame.seededCalldataPointers->insert(name);
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
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	std::vector<Type const*> types;
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (auto const& [name, type]: _params)
	{
		auto value = awst::makeVarExpression(name, type, _loc);
		if (auto const* source = calldataSolType(name); solTypeUsable(source))
		{
			types.push_back(source);
			values.push_back(std::move(value));
			continue;
		}
		// Storage handles and genuinely synthetic parameters are not calldata
		// declarations. Retain their explicit word representation, without
		// pretending that solc described this target-specific transport.
		if (isDynamicCalldataType(type))
		{
			Logger::instance().error("cannot derive EVM calldata layout for a synthetic dynamic parameter", _loc);
			return;
		}
		for (int i = 0; i < computeFlatElementCount(type); ++i)
		{
			types.push_back(TypeProvider::uint256());
			values.push_back(ensureBiguint(accessFlatElement(value, type, i, _loc), _loc));
		}
	}
	if (!codec::canRoundTripEvmAbi(types))
	{
		Logger::instance().error("unsupported Solidity calldata encoding", _loc);
		return;
	}
	auto body = abi::encodeEvmAbi(m_typeMapper, types, std::move(values), _loc, _out);
	auto hasArgs = awst::makeNumericCompare(
		awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), _loc),
		awst::NumericComparison::Gt, awst::makeZero(_loc), _loc);
	std::shared_ptr<awst::Expression> selector = awst::makeConditional(
		std::move(hasArgs), awst::makeAppArg(0, _loc), awst::makeBzero(4, _loc),
		awst::WType::bytesType(), _loc);
	if (m_typeMapper.profile().evmSelectors)
		selector = SelectorSemantics::translateRuntimeSelector(
			std::move(selector), m_context->selectorRoutes, _loc);
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), _loc),
		awst::makeConcat(std::move(selector), std::move(body), _loc), _loc));
	m_frame.locals[CD_BLOB_VAR] = awst::WType::bytesType();
}

} // namespace puyasol::builder
