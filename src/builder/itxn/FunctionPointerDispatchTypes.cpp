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



} // namespace puyasol::builder::eb
