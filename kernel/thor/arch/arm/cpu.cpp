#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/cpu-data.hpp>
#include <frg/manual_box.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/ring-buffer.hpp>

namespace thor {

extern "C" void saveFpSimdRegisters(FpRegisters *frame);
extern "C" void restoreFpSimdRegisters(FpRegisters *frame);

bool FaultImageAccessor::allowUserPages() {
	return true;
}

void UserContext::deactivate() { }
UserContext::UserContext()
: kernelStack(UniqueKernelStack::make()) { }

void UserContext::migrate(CpuData *cpu_data) {
	assert(!intsAreEnabled());
	cpu_data->exceptionStackPtr = kernelStack.basePtr();
}

FiberContext::FiberContext(UniqueKernelStack stack)
: stack{std::move(stack)} { }

extern "C" [[ noreturn ]] void _restoreExecutorRegisters(void *pointer);

[[noreturn]] void restoreExecutor(Executor *executor) {
	getCpuData()->currentDomain = static_cast<uint64_t>(executor->general()->domain);
	getCpuData()->exceptionStackPtr = executor->_exceptionStack;
	restoreFpSimdRegisters(&executor->general()->fp);
	_restoreExecutorRegisters(executor->general());
}

size_t Executor::determineSize() {
	return sizeof(Frame);
}

Executor::Executor()
: _pointer{nullptr}, _exceptionStack{nullptr} {  }

Executor::Executor(UserContext *context, AbiParameters abi) {
	_pointer = static_cast<char *>(kernelAlloc->allocate(getStateSize()));
	memset(_pointer, 0, getStateSize());

	general()->elr = abi.ip;
	general()->sp = abi.sp;
	general()->spsr = 0;
	general()->domain = Domain::user;

	_exceptionStack = context->kernelStack.basePtr();
}

Executor::Executor(FiberContext *context, AbiParameters abi)
: _exceptionStack{nullptr} {
	_pointer = static_cast<char *>(kernelAlloc->allocate(getStateSize()));
	memset(_pointer, 0, getStateSize());

	general()->elr = abi.ip;
	general()->sp = (uintptr_t)context->stack.basePtr();
	general()->x[0] = abi.argument;
	general()->spsr = isKernelInEl2() ? 9 : 5;
	general()->domain = Domain::fiber;
}

Executor::~Executor() {
	kernelAlloc->free(_pointer);
}

void saveExecutor(Executor *executor, FaultImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->domain = accessor._frame()->domain;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;

	saveFpSimdRegisters(&executor->general()->fp);
}

void saveExecutor(Executor *executor, IrqImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->domain = accessor._frame()->domain;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;

	saveFpSimdRegisters(&executor->general()->fp);
}

void saveExecutor(Executor *executor, SyscallImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->domain = accessor._frame()->domain;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;

	saveFpSimdRegisters(&executor->general()->fp);
}

extern "C" void workStub();

void workOnExecutor(Executor *executor) {
	auto sp = reinterpret_cast<uint64_t *>(executor->getExceptionStack());
	auto push = [&] (uint64_t v) {
		sp -= 2;
		memcpy(sp, &v, 8);
	};

	assert(executor->general()->domain == Domain::user);
	assert(getCpuData()->currentDomain != static_cast<uint64_t>(Domain::user));

	push(static_cast<uint64_t>(executor->general()->domain));
	push(executor->general()->sp);
	push(executor->general()->elr);
	push(executor->general()->spsr);

	void *stub = reinterpret_cast<void *>(&workStub);
	executor->general()->domain = Domain::fault;
	executor->general()->elr = reinterpret_cast<uintptr_t>(stub);
	executor->general()->sp = reinterpret_cast<uintptr_t>(sp);
	executor->general()->spsr = 0x3c0 | (isKernelInEl2() ? 9 : 5);
}

void scrubStack(FaultImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);;
}

void scrubStack(IrqImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);;
}

void scrubStack(SyscallImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);;
}

void scrubStack(Executor *executor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(*executor->sp()), cont);
}

size_t getStateSize() {
	return Executor::determineSize();
}

PlatformCpuData::PlatformCpuData() {
}

// TODO: support PAN?
void enableUserAccess() { }
void disableUserAccess() { }

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

void doRunOnStack(void (*function) (void *, void *), void *sp, void *argument) {
	assert(!intsAreEnabled());

	cleanKasanShadow(reinterpret_cast<std::byte *>(sp) - UniqueKernelStack::kSize,
			UniqueKernelStack::kSize);

	asm volatile (
			"\tmov x28, sp\n"
			"\tmov x1, sp\n"
			"\tmov x0, %0\n"
			"\tmov sp, %2\n"
			"\tblr %1\n"
			"\tmov sp, x28\n"
			:
			: "r" (argument), "r" (function), "r" (sp)
			: "x30", "x28", "x1", "x0", "memory");
}

Error getEntropyFromCpu(void *buffer, size_t size) { return Error::noHardwareSupport; }

namespace {
	constinit frg::manual_box<ReentrantRecordRing> bootLogRing;
}

void setupCpuContext(AssemblyCpuData *context) {
	asm volatile("msr tpidr_el1, %0" :: "r"(context));
}

void prepareCpuDataFor(CpuData *context, int cpu) {
	cpuData.initialize(context);

	context->selfPointer = context;
	context->cpuIndex = cpu;
}

void setupBootCpuContext() {
	auto context = &cpuData.getFor(0);

	prepareCpuDataFor(context, 0);
	setupCpuContext(context);

	bootLogRing.initialize();
	cpuData.get().localLogRing = bootLogRing.get();
}

static initgraph::Task initBootProcessorTask{&globalInitEngine, "arm.init-boot-processor",
	initgraph::Entails{getBootProcessorReadyStage()},
	[] {
		infoLogger() << "Booting on CPU #0" << frg::endlog;

		if (isKernelInEl2()) {
			infoLogger() << "Booting in EL2" << frg::endlog;
		} else {
			infoLogger() << "Booting in EL1" << frg::endlog;
		}

		initializeThisProcessor();
	}
};

initgraph::Stage *getBootProcessorReadyStage() {
	static initgraph::Stage s{&globalInitEngine, "arm.boot-processor-ready"};
	return &s;
}

initgraph::Edge bootProcessorReadyEdge{
	getBootProcessorReadyStage(),
	getFibersAvailableStage()
};

void initializeThisProcessor() {
	auto cpu_data = getCpuData();

	// Enable FPU
	asm volatile ("msr cpacr_el1, %0" :: "r"(uint64_t(0b11 << 20)));

	// Enable access to cache info register and cache maintenance instructions
	uint64_t sctlr;
	asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));

	sctlr |= (uint64_t(1) << 14);
	sctlr |= (uint64_t(1) << 15);
	sctlr |= (uint64_t(1) << 26);

	asm volatile ("msr sctlr_el1, %0" :: "r"(sctlr));

	uint64_t mpidr;
	asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	cpu_data->affinity = (mpidr & 0xFFFFFF) | (mpidr >> 32 & 0xFF) << 24;

	cpu_data->irqStack = UniqueKernelStack::make();
	cpu_data->detachedStack = UniqueKernelStack::make();
	cpu_data->idleStack = UniqueKernelStack::make();

	cpu_data->irqStackPtr = cpu_data->irqStack.basePtr();

	cpu_data->wqFiber = KernelFiber::post([] {
		// Do nothing. Our only purpose is to run the associated work queue.
	});
	cpu_data->generalWorkQueue = cpu_data->wqFiber->associatedWorkQueue()->selfPtr.lock();
	assert(cpu_data->generalWorkQueue);
}

} // namespace thor
