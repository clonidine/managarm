#include <eir-internal/arch.hpp>
#include <eir-internal/arch/riscv.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>

namespace eir {

namespace {

constexpr uint64_t pteValid = UINT64_C(1) << 0;
constexpr uint64_t pteRead = UINT64_C(1) << 1;
constexpr uint64_t pteWrite = UINT64_C(1) << 2;
constexpr uint64_t pteExecute = UINT64_C(1) << 3;
constexpr uint64_t pteGlobal = UINT64_C(1) << 5;
constexpr uint64_t pteAccess = UINT64_C(1) << 6;
constexpr uint64_t pteDirty = UINT64_C(1) << 7;
constexpr uint64_t ptePpnMask = (((UINT64_C(1) << 44) - 1) << 10);

} // namespace

physaddr_t pml4;

void
mapSingle4kPage(address_t address, address_t physical, uint32_t flags, CachingMode caching_mode) {
	assert(!(address & (pageSize - 1)));
	assert(!(physical & (pageSize - 1)));
	(void)caching_mode;

	// This needs to be determined before mapSingle4k() is called.
	assert(riscvConfig.numPtLevels);

	auto *table = physToVirt<uint64_t>(pml4);
	for (int i = 1; i < riscvConfig.numPtLevels; ++i) {
		// Iteration i handles VPN[n].
		// Sv39: n = 2, 1.
		// Sv48: n = 3, 2, 1.
		int n = riscvConfig.numPtLevels - i;
		// VPN[3] starts at 39.
		// VPN[2] starts at 30.
		// VPN[1] starts at 21.
		int shift = 12 + 9 * n;
		unsigned int vpn = (address >> shift) & 0x1FF;

		if (table[vpn] & pteValid) {
			table = physToVirt<uint64_t>((table[vpn] & ptePpnMask) << 2);
		} else {
			auto nextPtPage = allocPage();

			auto *nextPtPtr = physToVirt<uint64_t>(nextPtPage);
			for (int j = 0; j < 512; j++)
				nextPtPtr[j] = 0;

			table[vpn] = (nextPtPage >> 2) | pteValid;
			table = nextPtPtr;
		}
	}

	// VPN[0] starts at 12.
	unsigned int vpn0 = (address >> 12) & 0x1FF;
	uint64_t pte0 = (physical >> 2) | pteValid | pteRead | pteAccess;
	if (flags & PageFlags::write)
		pte0 |= pteWrite | pteDirty;
	if (flags & PageFlags::execute)
		pte0 |= pteExecute;
	if (flags & PageFlags::global)
		pte0 |= pteGlobal;
	table[vpn0] = pte0;
}

int getKernelVirtualBits() {
	assert(riscvConfig.numPtLevels);
	return 9 * riscvConfig.numPtLevels + 12;
}

void initProcessorPaging() {
	pml4 = allocPage();

	auto pml4Virtual = physToVirt<uint64_t>(pml4);
	for (int i = 0; i < 512; i++)
		pml4Virtual[i] = 0;

	for (int i = 256; i < 512; i++) {
		uintptr_t pml3Page = allocPage();

		uint64_t *pml3Ptr = physToVirt<uint64_t>(pml3Page);
		for (int j = 0; j < 512; j++)
			pml3Ptr[j] = 0;

		pml4Virtual[i] = (pml3Page >> 2) | pteValid;
	}

	eir::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10)
	                  << " KiB"
	                     " after setting up paging"
	                  << frg::endlog;

	// PE doesn't support linker scripts, this needs to be worked around by UEFI
	// see the `uefi.map-eir-image` task
#if !defined(EIR_UEFI)
	auto floor = reinterpret_cast<address_t>(&eirImageFloor) & ~address_t{0xFFF};
	auto ceiling = (reinterpret_cast<address_t>(&eirImageCeiling) + 0xFFF) & ~address_t{0xFFF};

	for (address_t addr = floor; addr < ceiling; addr += 0x1000)
		if (kernel_physical != SIZE_MAX) {
			mapSingle4kPage(
			    addr, addr - floor + kernel_physical, PageFlags::write | PageFlags::execute
			);
		} else {
			mapSingle4kPage(addr, addr, PageFlags::write | PageFlags::execute);
		}
#endif

	mapRegionsAndStructs();
}

} // namespace eir
