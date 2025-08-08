#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>

#include <frg/tuple.hpp>
#include <frg/vector.hpp>
#include <initgraph.hpp>
#include <thor-internal/arch-generic/cpu-data.hpp>
#include <thor-internal/arch/asm.h>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-stack.hpp>
#include <thor-internal/types.hpp>

#include <thor-internal/arch-generic/asid.hpp>

namespace thor {

enum class Domain : uint64_t { irq = 0, fault, fiber, user, idle };

struct Frame {
	uint64_t xs[31]; // X0 is constant zero, no need to save it.
	uint64_t ip;
	// Note: this is a subset of the sstatus CSR that should be *restored* by sret.
	//       Care must be taken when synthesizing a value for this from kernel space.
	//       For example, to ensure that interrupts are disabled, "spie" (not "sie") must be set.
	uint64_t sstatus;

	constexpr uint64_t &x(unsigned int n) {
		assert(n > 0 && n <= 31);
		return xs[n - 1];
	}

	constexpr uint64_t &a(unsigned int n) {
		assert(n <= 7);
		return x(10 + n);
	}
	constexpr uint64_t &ra() { return x(1); }
	constexpr uint64_t &sp() { return x(2); }
	constexpr uint64_t &tp() { return x(4); }

	bool umode() { return !(sstatus & riscv::sstatus::sppBit); }
	// Note: Returns "spie" (not "sie"), see above.
	bool sie() { return sstatus & riscv::sstatus::spieBit; }
};
static_assert(offsetof(Frame, ip) == 0xF8);
static_assert(sizeof(Frame) == 0x108);

struct Executor;

struct Continuation {
	void *sp;
};

struct SyscallImageAccessor {
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);

	SyscallImageAccessor(Frame *ptr) : _pointer{ptr} {}

	// The "- 1" is since we do not save x0
	// this makes the first number the register ID
	// We begin from A0
	// in7 and in8 are actually S2 and S3, since (according to
	// the calling convention) not enough argument registers
	Word *number() { return &frame()->xs[10 - 1]; }
	Word *in0() { return &frame()->xs[11 - 1]; }
	Word *in1() { return &frame()->xs[12 - 1]; }
	Word *in2() { return &frame()->xs[13 - 1]; }
	Word *in3() { return &frame()->xs[14 - 1]; }
	Word *in4() { return &frame()->xs[15 - 1]; }
	Word *in5() { return &frame()->xs[16 - 1]; }
	Word *in6() { return &frame()->xs[17 - 1]; }
	Word *in7() { return &frame()->xs[18 - 1]; }
	Word *in8() { return &frame()->xs[19 - 1]; }

	Word *error() { return &frame()->xs[10 - 1]; }
	Word *out0() { return &frame()->xs[11 - 1]; }
	Word *out1() { return &frame()->xs[12 - 1]; }

	Frame *frame() { return _pointer; }

	void *frameBase() { return reinterpret_cast<char *>(_pointer) + sizeof(Frame); }

private:
	Frame *_pointer;
};

struct FaultImageAccessor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);

	FaultImageAccessor(Frame *frame) : _pointer{frame} {}

	Word *ip() { return &frame()->ip; }
	Word *sp() { unimplementedOnRiscv(); }

	bool inKernelDomain() { return !frame()->umode(); }

	bool allowUserPages() { return frame()->sstatus & riscv::sstatus::sumBit; }

	Frame *frame() { return _pointer; }

	void *frameBase() { return reinterpret_cast<char *>(_pointer) + sizeof(Frame); }

private:
	Frame *_pointer;
};

struct IrqImageAccessor {
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);

	explicit IrqImageAccessor(Frame *frame) : _pointer{frame} {}

	Word *ip() { unimplementedOnRiscv(); }
	Word *rflags() { unimplementedOnRiscv(); }

	bool inPreemptibleDomain() { return frame()->umode() || frame()->sie(); }

	bool inManipulableDomain() { return frame()->umode(); }

	bool inThreadDomain() { unimplementedOnRiscv(); }

	bool inFiberDomain() { unimplementedOnRiscv(); }

	bool inIdleDomain() { unimplementedOnRiscv(); }

	Frame *frame() { return _pointer; }

	void *frameBase() { return reinterpret_cast<char *>(_pointer) + sizeof(Frame); }

private:
	Frame *_pointer;
};

// CpuData is some high-level struct that inherits from PlatformCpuData.
struct CpuData;

struct AbiParameters {
	uintptr_t ip;
	uintptr_t sp;
	uintptr_t argument;
};

struct UserContext {
	static void deactivate();

	UserContext();

	UserContext(const UserContext &other) = delete;

	UserContext &operator=(const UserContext &other) = delete;

	// Migrates this UserContext to a different CPU.
	void migrate(CpuData *cpuData);

	// TODO: This should be private.
	UniqueKernelStack kernelStack;
};

struct FiberContext {
	FiberContext(UniqueKernelStack s) : stack{std::move(s)} {}

	FiberContext(const FiberContext &other) = delete;

	FiberContext &operator=(const FiberContext &other) = delete;

	// TODO: This should be private.
	UniqueKernelStack stack;
};

struct Executor;

// Restores the current executor from its saved image.
// This function does the heavy lifting during task switch.
// Note: due to the attribute, this must be declared before the friend declaration below.
[[noreturn]] void restoreExecutor(Executor *executor);

struct Executor {
	// 32 FP registers (32 * 8 bytes) + FCSR (8 bytes).
	// TODO: This hardcodes the size of the FP state (RISC-V allows 32/64/128 bit).
	constexpr static size_t fpStateSize = 32 * sizeof(uint64_t) + sizeof(uint64_t);

	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);
	friend void saveCurrentSimdState(Executor *executor);
	friend void workOnExecutor(Executor *executor);
	friend void restoreExecutor(Executor *executor);

	static size_t determineSize() { return sizeof(Frame) + fpStateSize; }
	// Offset (relative to _pointer) of f0-f31 and fcsr (in this order).
	static size_t fsOffset() { return sizeof(Frame); }

	explicit Executor(UserContext *context, AbiParameters abi);
	explicit Executor(FiberContext *context, AbiParameters abi);

	Executor(const Executor &other) = delete;

	~Executor();

	Executor &operator=(const Executor &other) = delete;

	Word *ip() { return &general()->ip; }
	Word *sp() { return &general()->sp(); }

	// Note: a0 is used for the supercall code.
	Word *arg0() { return &general()->a(1); }
	Word *arg1() { return &general()->a(2); }
	Word *result0() { return &general()->a(0); }
	Word *result1() { return &general()->a(1); }

	Frame *general() { return reinterpret_cast<Frame *>(_pointer); }

	void *getExceptionStack() { return _exceptionStack; }

	void *fpRegisters() { return _pointer + fsOffset(); }

	UserAccessRegion *currentUar() { return _uar; }

private:
	// Private function only used for the static_assert check.
	//
	// We can't put the static_assert outside because the members are private
	// and we can't put it at the end of the struct body because the type
	// is incomplete at that point.
	static void staticChecks() {
		static_assert(offsetof(Executor, _uar) == THOR_EXECUTOR_UAR);
	}
	
	char *_pointer{nullptr};
	void *_exceptionStack{nullptr};
	UserAccessRegion *_uar{nullptr};
};

size_t getStateSize();

// Determine whether this address belongs to the higher half.
inline constexpr bool inHigherHalf(uintptr_t address) {
	return address & (static_cast<uintptr_t>(1) << 63);
}

void bootSecondary(unsigned int apic_id);

size_t getCpuCount();

void saveCurrentSimdState(Executor *executor);

void prepareCpuDataFor(CpuData *context, int cpu);
void setupBootCpuContext();
void initializeThisProcessor();

initgraph::Stage *getBootProcessorReadyStage();

} // namespace thor
