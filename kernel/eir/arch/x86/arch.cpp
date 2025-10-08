#include <arch/io_space.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/memory-layout.hpp>
#include <x86/gdt.hpp>
#include <x86/machine.hpp>

extern "C" [[noreturn]] void
eirEnterKernel(uintptr_t pml4Pointer, uint64_t entryPtr, uint64_t stackPtr)
    __attribute__((sysv_abi));

namespace eir {

void debugPrintChar(char c) {
	if (log_e9) {
		static constexpr arch::scalar_register<uint8_t> data{0};
		auto base = arch::global_io.subspace(0xe9);

		base.store(data, c);
	}
}

void initPlatform() {}

enum X86PageFlags {
	kPagePresent = 1,
	kPageWrite = 2,
	kPageUser = 4,
	kPagePwt = 0x8,
	kPagePat = 0x80,
	kPageGlobal = 0x100,
	kPageXd = 0x8000000000000000
};

uintptr_t eirPml4Pointer = 0;

void setupPaging() {
	eirPml4Pointer = allocPage();
	auto pml4Virtual = physToVirt<uint64_t>(eirPml4Pointer);

	for (int i = 0; i < 512; i++)
		pml4Virtual[i] = 0;

	for (int i = 256; i < 512; i++) {
		uintptr_t pdpt_page = allocPage();
		uint64_t *pdpt_pointer = physToVirt<uint64_t>(pdpt_page);
		for (int j = 0; j < 512; j++)
			pdpt_pointer[j] = 0;

		pml4Virtual[i] = pdpt_page | kPagePresent | kPageWrite;
	}
}

void
mapSingle4kPage(address_t address, address_t physical, uint32_t flags, CachingMode caching_mode) {
	assert(address % pageSize == 0);
	assert(physical % pageSize == 0);

	int pml4_index = (int)((address >> 39) & 0x1FF);
	int pdpt_index = (int)((address >> 30) & 0x1FF);
	int pd_index = (int)((address >> 21) & 0x1FF);
	int pt_index = (int)((address >> 12) & 0x1FF);

	// find the pml4_entry. the pml4 is always present
	uintptr_t pml4 = eirPml4Pointer;
	uint64_t pml4_entry = (physToVirt<uint64_t>(pml4))[pml4_index];

	// find the pdpt entry; create pdpt if necessary
	uintptr_t pdpt = (uintptr_t)(pml4_entry & 0xFFFFF000);
	if (!(pml4_entry & kPagePresent)) {
		pdpt = allocPage();
		for (int i = 0; i < 512; i++)
			(physToVirt<uint64_t>(pdpt))[i] = 0;
		(physToVirt<uint64_t>(pml4))[pml4_index] = pdpt | kPagePresent | kPageWrite;
	}
	uint64_t pdpt_entry = (physToVirt<uint64_t>(pdpt))[pdpt_index];

	// find the pd entry; create pd if necessary
	uintptr_t pd = (uintptr_t)(pdpt_entry & 0xFFFFF000);
	if (!(pdpt_entry & kPagePresent)) {
		pd = allocPage();
		for (int i = 0; i < 512; i++)
			(physToVirt<uint64_t>(pd))[i] = 0;
		(physToVirt<uint64_t>(pdpt))[pdpt_index] = pd | kPagePresent | kPageWrite;
	}
	uint64_t pd_entry = (physToVirt<uint64_t>(pd))[pd_index];

	// find the pt entry; create pt if necessary
	uintptr_t pt = (uintptr_t)(pd_entry & 0xFFFFF000);
	if (!(pd_entry & kPagePresent)) {
		pt = allocPage();
		for (int i = 0; i < 512; i++)
			(physToVirt<uint64_t>(pt))[i] = 0;
		(physToVirt<uint64_t>(pd))[pd_index] = pt | kPagePresent | kPageWrite;
	}
	uint64_t pt_entry = (physToVirt<uint64_t>(pt))[pt_index];

	// setup the new pt entry
	if (pt_entry & kPagePresent)
		eir::panicLogger() << "eir: Trying to map 0x" << frg::hex_fmt{address} << " twice!"
		                   << frg::endlog;

	uint64_t new_entry = physical | kPagePresent;
	if (flags & PageFlags::write)
		new_entry |= kPageWrite;
	if (!(flags & PageFlags::execute))
		new_entry |= kPageXd;
	if (flags & PageFlags::global)
		new_entry |= kPageGlobal;
	if (caching_mode == CachingMode::writeCombine)
		new_entry |= kPagePat | kPagePwt;
	else
		assert(caching_mode == CachingMode::null);

	(physToVirt<uint64_t>(pt))[pt_index] = new_entry;
}

address_t getSingle4kPage(address_t address) {
	assert(address % pageSize == 0);

	int pml4_index = (int)((address >> 39) & 0x1FF);
	int pdpt_index = (int)((address >> 30) & 0x1FF);
	int pd_index = (int)((address >> 21) & 0x1FF);
	int pt_index = (int)((address >> 12) & 0x1FF);

	// find the pml4_entry. the pml4 is always present
	uintptr_t pml4 = eirPml4Pointer;
	uint64_t pml4_entry = (physToVirt<uint64_t>(pml4))[pml4_index];

	// find the pdpt entry; bail out if pdpt is missing
	uintptr_t pdpt = (uintptr_t)(pml4_entry & 0xFFFFF000);
	if (!(pml4_entry & kPagePresent))
		return -1;
	uint64_t pdpt_entry = (physToVirt<uint64_t>(pdpt))[pdpt_index];

	// find the pd entry; bail out if pd is missing
	uintptr_t pd = (uintptr_t)(pdpt_entry & 0xFFFFF000);
	if (!(pdpt_entry & kPagePresent))
		return -1;
	uint64_t pd_entry = (physToVirt<uint64_t>(pd))[pd_index];

	// find the pt entry; bail out if pt is missing
	uintptr_t pt = (uintptr_t)(pd_entry & 0xFFFFF000);
	if (!(pd_entry & kPagePresent))
		return -1;
	uint64_t pt_entry = (physToVirt<uint64_t>(pt))[pt_index];

	// setup the new pt entry
	if (!(pt_entry & kPagePresent))
		return -1;
	return pt_entry & 0xF'FFFF'FFFF'F000;
}

void initArchCpu();

void initProcessorEarly() {
	namespace arch = common::x86;

	eir::infoLogger() << "Starting Eir" << frg::endlog;

	frg::array<uint32_t, 4> vendor_res = arch::cpuid(0);
	char vendor_str[13];
	memcpy(&vendor_str[0], &vendor_res[1], 4);
	memcpy(&vendor_str[4], &vendor_res[3], 4);
	memcpy(&vendor_str[8], &vendor_res[2], 4);
	vendor_str[12] = 0;
	eir::infoLogger() << "CPU vendor: " << (const char *)vendor_str << frg::endlog;

	// Make sure everything we require is supported by the CPU.
	frg::array<uint32_t, 4> extended = arch::cpuid(arch::kCpuIndexExtendedFeatures);
	if ((extended[3] & arch::kCpuFlagLongMode) == 0)
		eir::panicLogger() << "Long mode is not supported on this CPU" << frg::endlog;
	if ((extended[3] & arch::kCpuFlagNx) == 0)
		eir::panicLogger() << "NX bit is not supported on this CPU" << frg::endlog;

	frg::array<uint32_t, 4> normal = arch::cpuid(arch::kCpuIndexFeatures);
	if ((normal[3] & arch::kCpuFlagPat) == 0)
		eir::panicLogger() << "PAT is not supported on this CPU" << frg::endlog;

	initArchCpu();

	// Program the PAT. Each byte configures a single entry.
	// 00: Uncacheable
	// 01: Write Combining
	// 04: Write Through
	// 06: Write Back
	// Keep in sync with the SMP trampoline in thor.
	uint64_t pat = 0x00'00'01'00'00'00'04'06;
	common::x86::wrmsr(0x277, pat);
}

int getKernelVirtualBits() { return 48; }

void initProcessorPaging() {
	setupPaging();
	eir::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10)
	                  << " KiB"
	                     " after setting up paging"
	                  << frg::endlog;
}

bool patchArchSpecificManagarmElfNote(unsigned int, frg::span<char>) { return false; }

[[noreturn]] void enterKernel() {
	eirEnterKernel(eirPml4Pointer, kernelEntry, getKernelStackPtr());
}

} // namespace eir
