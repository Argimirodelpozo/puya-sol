#pragma once

#include "awst/Node.h"

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace puyasol::builder::storage_dispatch
{

/// Move generated storage runtime methods out of the contract and expose them
/// as root subroutines. Library/free-function callers cannot target instance
/// methods, so both physical storage backends share this promotion boundary.
inline void promoteMethods(
	awst::Contract& _contract,
	std::vector<std::shared_ptr<awst::Subroutine>>& _destination,
	std::string const& _idPrefix,
	std::initializer_list<std::string_view> _names)
{
	std::vector<awst::ContractMethod> remainingMethods;
	for (auto& method: _contract.methods)
	{
		auto const selected = std::find(
			_names.begin(), _names.end(), method.memberName) != _names.end();
		if (selected)
			_destination.push_back(awst::makeSubroutine(
				_idPrefix + method.memberName, method.memberName,
				std::move(method.args), method.returnType, std::move(method.body),
				/*pure=*/false,
				method.sourceLocation));
		else
			remainingMethods.push_back(std::move(method));
	}
	_contract.methods = std::move(remainingMethods);
}

} // namespace puyasol::builder::storage_dispatch
