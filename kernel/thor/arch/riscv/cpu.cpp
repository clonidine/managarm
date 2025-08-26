#include <frg/manual_box.hpp>
#include <generic/thor-internal/cpu-data.hpp>
#include <riscv/sbi.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch/fp-state.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/ring-buffer.hpp>

namespace thor {

void enableUserAccess() { riscv::setCsrBits<riscv::Csr::sstatus>(riscv::sstatus::sumBit); }
void disableUserAccess() { riscv::clearCsrBits<riscv::Csr::sstatus>(riscv::sstatus::sumBit); }

bool iseqStore64(uint64_t *p, uint64_t v) {
	// TODO: This is a shim. A proper implementation is needed for NMIs on ARM.
	std::atomic_ref{*p}.store(v, std::memory_order_relaxed);
	return true;
}

bool iseqCopyWeak(void *dst, const void *src, size_t size) {
	// TODO: This is a shim. A proper implementation is needed for NMIs on ARM.
	memcpy(dst, src, size);
	return true;
}

UserContext::UserContext() : kernelStack(UniqueKernelStack::make()) {}

void UserContext::migrate(CpuData *) {
	assert(!intsAreEnabled());
	// TODO: ARM refreshes a pointer to the exception stack in CpuData here.
}

void UserContext::deactivate() {}

void saveExecutor(Executor *executor, FaultImageAccessor accessor) {
	saveCurrentSimdState(executor);
	memcpy(executor->general(), accessor.frame(), sizeof(Frame));
}
void saveExecutor(Executor *executor, IrqImageAccessor accessor) {
	saveCurrentSimdState(executor);
	memcpy(executor->general(), accessor.frame(), sizeof(Frame));
}
void saveExecutor(Executor *executor, SyscallImageAccessor accessor) {
	saveCurrentSimdState(executor);
	memcpy(executor->general(), accessor.frame(), sizeof(Frame));
}

void workOnExecutor(Executor *executor) {
	auto sp = reinterpret_cast<char *>(executor->getExceptionStack()) - sizeof(Frame);

	auto *userFrame = reinterpret_cast<Frame *>(sp);
	auto *kernelFrame = executor->general();
	memcpy(userFrame, kernelFrame, sizeof(Frame));

	auto *entry = reinterpret_cast<void *>(handleRiscvWorkOnExecutor);
	memset(kernelFrame->xs, 0, 31 * sizeof(uint64_t));
	kernelFrame->ip = reinterpret_cast<uintptr_t>(entry);
	kernelFrame->sp() = reinterpret_cast<uintptr_t>(sp);
	kernelFrame->a(0) = reinterpret_cast<uintptr_t>(executor);
	kernelFrame->a(1) = reinterpret_cast<uintptr_t>(userFrame);
	kernelFrame->sstatus |= riscv::sstatus::sppBit;
	kernelFrame->sstatus &= ~riscv::sstatus::spieBit;
	kernelFrame->sstatus &= ~(riscv::sstatus::extMask << riscv::sstatus::fsShift);
}

Executor::Executor(UserContext *context, AbiParameters abi) {
	size_t size = determineSize();
	_pointer = reinterpret_cast<char *>(kernelAlloc->allocate(size));
	memset(_pointer, 0, size);

	general()->ip = abi.ip;
	general()->sp() = abi.sp;
	// Note: we could use extInitial here.
	//       However, that would require changes in the restore code path to zero the registers.
	general()->sstatus = riscv::sstatus::extClean << riscv::sstatus::fsShift;

	_exceptionStack = context->kernelStack.basePtr();
}

Executor::Executor(FiberContext *context, AbiParameters abi) {
	size_t size = determineSize();
	_pointer = reinterpret_cast<char *>(kernelAlloc->allocate(size));
	memset(_pointer, 0, size);

	general()->ip = abi.ip;
	general()->sp() = (uintptr_t)context->stack.basePtr();
	general()->a(0) = abi.argument;
	general()->sstatus = riscv::sstatus::sppBit;
}

Executor::~Executor() { kernelAlloc->free(_pointer); }

void scrubStack(FaultImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);
	;
}

void scrubStack(IrqImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);
	;
}

void scrubStack(SyscallImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);
	;
}

void scrubStack(Executor *executor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(*executor->sp()), cont);
}

Error getEntropyFromCpu(void *buffer, size_t size) { return Error::noHardwareSupport; }

void doRunOnStack(void (*function)(void *, void *), void *sp, void *argument) {
	assert(!intsAreEnabled());

	cleanKasanShadow(
	    reinterpret_cast<std::byte *>(sp) - UniqueKernelStack::kSize, UniqueKernelStack::kSize
	);
	register uint64_t a0 asm("a0") = reinterpret_cast<uint64_t>(argument);
	// clang-format off
	asm volatile (
		     "mv a1, sp" "\n"
		"\t" "mv sp, %0" "\n"
		"\t" "jalr %1"   "\n"
		"\t" "unimp"
		:
		: "r"(sp), "r" (function), "r"(a0)
		: "a1", "memory"
	);
	// clang-format on
}

void saveCurrentSimdState(Executor *executor) {
	assert(!intsAreEnabled());
	auto cpuData = getCpuData();

	auto sstatus = riscv::readCsr<riscv::Csr::sstatus>();
	assert((sstatus & riscv::sstatus::sumBit) == 0);

	if (cpuData->stashedFs == riscv::sstatus::extDirty) {
		auto fs = reinterpret_cast<uint64_t *>(executor->_pointer + Executor::fsOffset());
		riscv::setCsrBits<riscv::Csr::sstatus>(riscv::sstatus::extDirty << riscv::sstatus::fsShift);
		fs[32] = riscv::readCsr<riscv::Csr::fcsr>();
		saveFpRegisters(fs);
		riscv::clearCsrBits<riscv::Csr::sstatus>(
		    riscv::sstatus::extMask << riscv::sstatus::fsShift
		);
	}
	// TODO: We can skip this for extInitial when we implement extInitial.
	cpuData->stashedFs = 0;
}

namespace {

constinit ReentrantRecordRing bootLogRing;

} // namespace

void initializeThisProcessor() {
	auto cpuData = getCpuData();

	// Initialize sstatus to a known state.
	auto sstatus = riscv::readCsr<riscv::Csr::sstatus>();
	// Disable floating point and vector extensions.
	sstatus &= ~(riscv::sstatus::extMask << riscv::sstatus::vsShift);
	sstatus &= ~(riscv::sstatus::extMask << riscv::sstatus::fsShift);
	sstatus &= ~(riscv::sstatus::extMask << riscv::sstatus::xsShift);
	// User-access is off. Executable pages are not always readable.
	sstatus &= ~riscv::sstatus::sumBit;
	sstatus &= ~riscv::sstatus::mxrBit;
	// U-mode is little endian and 64-bit.
	sstatus &= ~riscv::sstatus::ubeBit;
	sstatus &= ~(riscv::sstatus::uxlMask << riscv::sstatus::uxlShift);
	sstatus |= riscv::sstatus::uxl64 << riscv::sstatus::uxlShift;
	riscv::writeCsr<riscv::Csr::sstatus>(sstatus);

	auto senvcfg = riscv::readCsr<riscv::Csr::senvcfg>();
	senvcfg |= riscv::senvcfg::cbie | riscv::senvcfg::cbcfe;
	riscv::writeCsr<riscv::Csr::senvcfg>(senvcfg);

	// Read back sstatus.
	sstatus = riscv::readCsr<riscv::Csr::sstatus>();
	if (sstatus & riscv::sstatus::ubeBit)
		panicLogger() << "thor: kernel does not support big endian userspace" << frg::endlog;
	if (((sstatus >> riscv::sstatus::uxlShift) & riscv::sstatus::uxlMask) != riscv::sstatus::uxl64)
		panicLogger() << "thor: kernel only supports 64-bit userspace" << frg::endlog;

	// Kernel mode runs with zero in sscratch.
	// User mode runs with the kernel tp in sscratch.
	riscv::writeCsr<riscv::Csr::sscratch>(0);

	cpuData->irqStack = UniqueKernelStack::make();
	cpuData->detachedStack = UniqueKernelStack::make();
	cpuData->idleStack = UniqueKernelStack::make();

	cpuData->irqStackPtr = cpuData->irqStack.basePtr();

	// Install the exception handler after stacks are set up.
	auto stvec = reinterpret_cast<uint64_t>(reinterpret_cast<const void *>(thorExceptionEntry));
	assert(!(stvec & 3));
	riscv::writeCsr<riscv::Csr::stvec>(stvec);

	// Enable the interrupts that we care about.
	riscv::writeCsr<riscv::Csr::sie>(
	    (UINT64_C(1) << riscv::interrupts::ssi) | (UINT64_C(1) << riscv::interrupts::sti)
	    | (UINT64_C(1) << riscv::interrupts::sei)
	);

	// Setup the per-CPU work queue.
	cpuData->wqFiber = KernelFiber::post([] {
		// Do nothing. Our only purpose is to run the associated work queue.
	});
	cpuData->generalWorkQueue = cpuData->wqFiber->associatedWorkQueue()->selfPtr.lock();
	assert(cpuData->generalWorkQueue);
}

void prepareCpuDataFor(CpuData *context, int cpu) {
	cpuData.initialize(context);

	context->selfPointer = context;
	context->cpuIndex = cpu;
}

void setupBootCpuContext() {
	auto context = &cpuData.getFor(0);

	prepareCpuDataFor(context, 0);
	writeToTp(context);

	cpuData.get().localLogRing = &bootLogRing;
	cpuData.get().hartId = getEirInfo()->hartId;
}

static initgraph::Task probeSbiFeatures{
    &globalInitEngine,
    "riscv.probe-sbi-features",
    initgraph::Entails{getFibersAvailableStage()},
    [] {
	    if (!sbi::base::probeExtension(sbi::eidIpi))
		    panicLogger() << "SBI does not implement IPI extension" << frg::endlog;
	    if (!riscvHartCapsNote->hasExtension(RiscvExtension::sstc)
	        && !sbi::base::probeExtension(sbi::eidTime))
		    panicLogger() << "SBI does not implement TIME extension" << frg::endlog;
    }
};

static initgraph::Task initBootProcessorTask{
    &globalInitEngine,
    "riscv.init-boot-processor",
    initgraph::Entails{getFibersAvailableStage()},
    [] {
	    debugLogger() << "Booting on HART " << cpuData.get().hartId << frg::endlog;
	    initializeThisProcessor();
    }
};

} // namespace thor
