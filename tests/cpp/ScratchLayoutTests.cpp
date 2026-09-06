#include "builder/ScratchLayout.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace
{

bool require(bool _condition, char const* _message)
{
	if (_condition)
		return true;
	std::cerr << _message << '\n';
	return false;
}

} // namespace

int main()
{
	using puyasol::builder::ScratchLayout;
	bool ok = true;

	ScratchLayout defaults;
	ok &= require(defaults.memoryFirst() == 0 && defaults.memoryLast() == 4,
		"default memory layout changed");
	ok &= require(defaults.reservedSlots().size() == 16,
		"default reservations are incomplete");

	ScratchLayout extended{7};
	ok &= require(extended.memoryFirst() == 0 && extended.memoryLast() == 6,
		"extended memory is not contiguous from slot zero");
	ok &= require(extended.transientSlot() == 7
			&& extended.flashFirst() == 8 && extended.flashLast() == 17,
		"extended reservations do not immediately follow memory");
	auto extendedSlots = extended.reservedSlots();
	bool contiguous = extendedSlots.size() == 18;
	for (size_t slot = 0; contiguous && slot < extendedSlots.size(); ++slot)
		contiguous = extendedSlots[slot] == static_cast<int>(slot);
	ok &= require(contiguous, "extended reservations are not contiguous");

	ok &= require(ScratchLayout::maxMemorySlots == 88,
		"maximum memory-slot policy changed");
	ScratchLayout maximum{88};
	ok &= require(maximum.memoryLast() == 87
			&& maximum.flashLast() == 98,
		"maximum CLI layout does not preserve its reserved slots");
	ok &= require(ScratchLayout::transientAddressShadowSlot > maximum.flashLast()
		&& ScratchLayout::transientAddressShadowSlot < 100,
		"transient address shadow overlaps memory/flash or splitter slots");

	for (int invalid: {0, 89})
	{
		bool rejected = false;
		try { ScratchLayout layout{invalid}; (void)layout; }
		catch (std::invalid_argument const&) { rejected = true; }
		ok &= require(rejected, "invalid scratch layout was accepted");
	}

	return ok ? 0 : 1;
}
