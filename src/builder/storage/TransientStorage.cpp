#include "builder/storage/TransientStorage.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

namespace puyasol::builder
{

void TransientStorage::collectVars(
	solidity::frontend::ContractDefinition const& _contract,
	TypeMapper& _typeMapper)
{
	m_vars.clear();
	m_offsetMap.clear();

	std::set<std::string> seen;
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		for (auto const* var: base->stateVariables())
		{
			if (var->isConstant())
				continue;
			if (var->referenceLocation() != solidity::frontend::VariableDeclaration::Location::Transient)
				continue;
			if (seen.count(var->name()))
				continue;
			seen.insert(var->name());

			if (m_vars.size() >= MAX_SLOTS)
			{
				Logger::instance().warning(
					"transient variable '" + var->name() + "' exceeds max slots ("
					+ std::to_string(MAX_SLOTS) + "), skipped");
				continue;
			}

			unsigned offset = static_cast<unsigned>(m_vars.size()) * SLOT_SIZE;
			auto* wtype = _typeMapper.map(var->type());
			m_vars.push_back({var->name(), offset, wtype});
			m_offsetMap[var->name()] = static_cast<unsigned>(m_vars.size() - 1);
		}
	}
}

bool TransientStorage::isTransient(solidity::frontend::VariableDeclaration const& _var) const
{
	return m_offsetMap.count(_var.name()) > 0;
}

int TransientStorage::getOffset(std::string const& _name) const
{
	auto it = m_offsetMap.find(_name);
	if (it == m_offsetMap.end())
		return -1;
	return static_cast<int>(m_vars[it->second].offset);
}

namespace
{
	std::shared_ptr<awst::Expression> loadTransientBlob(awst::SourceLocation const& _loc)
	{
		auto loadOp = awst::makeIntrinsicCall("load", awst::WType::bytesType(), _loc);
		loadOp->immediates = {AssemblyBuilder::TRANSIENT_SLOT};
		return loadOp;
	}
}

std::shared_ptr<awst::Expression> TransientStorage::buildRead(
	std::string const& _name, awst::WType const* _type,
	awst::SourceLocation const& _loc) const
{
	int offset = getOffset(_name);
	if (offset < 0)
		return nullptr;

	// Load blob from scratch slot TRANSIENT_SLOT (backed across callsub).
	auto blob = loadTransientBlob(_loc);

	// extract(blob, offset, 32) → 32 raw bytes
	auto extract = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
	extract->immediates = {offset, static_cast<int>(SLOT_SIZE)};
	extract->stackArgs.push_back(std::move(blob));

	// Reinterpret bytes as the target type
	if (_type == awst::WType::biguintType())
	{
		auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);
		return cast;
	}
	else if (_type == awst::WType::uint64Type() || _type == awst::WType::boolType())
	{
		// extract last 8 bytes → btoi
		auto extract8 = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
		extract8->immediates = {offset + static_cast<int>(SLOT_SIZE) - 8, 8};

		auto blobRef = loadTransientBlob(_loc);
		extract8->stackArgs.push_back(std::move(blobRef));

		auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
		btoi->stackArgs.push_back(std::move(extract8));
		return btoi;
	}

	// Default: return raw bytes
	return extract;
}

std::shared_ptr<awst::Statement> TransientStorage::buildWrite(
	std::string const& _name, std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc) const
{
	int offset = getOffset(_name);
	if (offset < 0)
		return nullptr;

	// Convert value to exactly 32 bytes (big-endian, left-padded)
	std::shared_ptr<awst::Expression> bytes32;
	if (_value->wtype == awst::WType::biguintType())
	{
		// biguint → bytes, then b| with bzero(32) to pad to ≥32 bytes,
		// then extract last 32
		auto toBytes = awst::makeReinterpretCast(std::move(_value), awst::WType::bytesType(), _loc);

		// bzero(32)
		auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		auto sz = awst::makeIntegerConstant("32", _loc);
		zeros->stackArgs.push_back(std::move(sz));

		// b| pads shorter operand to match longer (both become 32+ bytes)
		auto padded = awst::makeIntrinsicCall("b|", awst::WType::bytesType(), _loc);
		padded->stackArgs.push_back(std::move(zeros));
		padded->stackArgs.push_back(std::move(toBytes));

		// extract last 32 bytes: extract(padded, len-32, 32)
		// But b| output is max(len_a, len_b) which is exactly 32 if val ≤ 32 bytes
		// For safety, just use the b| result directly (it's 32 bytes for values ≤ 256 bits)
		bytes32 = std::move(padded);
	}
	else if (_value->wtype == awst::WType::uint64Type()
		|| _value->wtype == awst::WType::boolType())
	{
		// itob produces 8 bytes, pad to 32 with bzero(24) prefix
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		itob->stackArgs.push_back(std::move(_value));

		auto prefix = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		auto twentyFour = awst::makeIntegerConstant("24", _loc);
		prefix->stackArgs.push_back(std::move(twentyFour));

		auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
		cat->stackArgs.push_back(std::move(prefix));
		cat->stackArgs.push_back(std::move(itob));

		bytes32 = std::move(cat);
	}
	else
	{
		bytes32 = std::move(_value);
	}

	// replace2(load TRANSIENT_SLOT, bytes32) at compile-time offset
	auto blobRead = loadTransientBlob(_loc);

	auto replace = awst::makeIntrinsicCall("replace2", awst::WType::bytesType(), _loc);
	replace->immediates = {offset};
	replace->stackArgs.push_back(std::move(blobRead));
	replace->stackArgs.push_back(std::move(bytes32));

	// store TRANSIENT_SLOT ← replace2(...)
	auto storeOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), _loc);
	storeOp->immediates = {AssemblyBuilder::TRANSIENT_SLOT};
	storeOp->stackArgs.push_back(std::move(replace));

	auto stmt = awst::makeExpressionStatement(std::move(storeOp), _loc);
	return stmt;
}

} // namespace puyasol::builder
