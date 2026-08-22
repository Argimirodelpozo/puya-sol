#include "builder/itxn/FunctionPointerDispatchTypes.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeMapper.h"
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

awst::WType const* computeReturnType(ContractContext& _ctx, FunctionType const* _funcType)
{
	if (!_funcType || _funcType->returnParameterTypes().empty())
		return awst::WType::voidType();
	auto const& rts = _funcType->returnParameterTypes();
	if (rts.size() == 1)
		return _ctx.typeMapper.map(rts[0]);
	std::vector<awst::WType const*> retTypes;
	for (auto const* rt : rts)
		retTypes.push_back(_ctx.typeMapper.map(rt));
	return _ctx.typeMapper.createType<awst::WTuple>(std::move(retTypes), std::nullopt);
}

awst::WType const* dispatchPublicArgArc4Type(
	TypeMapper& _typeMapper,
	awst::WType const* _nativeType,
	solidity::frontend::Type const* _paramSolType)
{
	if (_nativeType == awst::WType::biguintType())
	{
		unsigned bits = 256;
		if (auto const* intType = dynamic_cast<IntegerType const*>(_paramSolType))
			bits = intType->numBits();
		return _typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
	}
	if (_nativeType && _nativeType->kind() == awst::WTypeKind::Bytes
		&& dynamic_cast<FunctionType const*>(_paramSolType))
	{
		// External fn-ptr bytes[N] → arc4.static_array<arc4.uint8, N>.
		// Mirrors ContractBuilder's rule: ARC4-remap bytes[N] only for FunctionType params.
		auto const* bytesType = static_cast<awst::BytesWType const*>(_nativeType);
		if (bytesType->length().has_value())
		{
			auto const* arc4Byte = _typeMapper.createType<awst::ARC4UIntN>(8);
			return _typeMapper.createType<awst::ARC4StaticArray>(
				arc4Byte, bytesType->length().value());
		}
	}
	return nullptr;
}

} // namespace puyasol::builder::eb
