
#include <signal.h>
#include <string.h>

#include "common.hpp"
#include <core/clock.hpp>
#include "exec.hpp"
#include "gdbserver.hpp"
#include "process.hpp"

#include <protocols/posix/data.hpp>

#include "debug-options.hpp"

static bool logFileAttach = false;
static bool logCleanup_ = false;

async::result<void> serve(std::shared_ptr<Process> self, std::shared_ptr<Generation> generation);

// ----------------------------------------------------------------------------
// VmContext.
// ----------------------------------------------------------------------------

std::shared_ptr<VmContext> VmContext::create() {
	auto context = std::make_shared<VmContext>();

	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	context->_space = helix::UniqueDescriptor(space);

	return context;
}

std::shared_ptr<VmContext> VmContext::clone(std::shared_ptr<VmContext> original) {
	auto context = std::make_shared<VmContext>();

	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	context->_space = helix::UniqueDescriptor(space);

	for(const auto &entry : original->_areaTree) {
		const auto &[address, area] = entry;

		helix::UniqueDescriptor copyView;
		if(area.copyOnWrite) {
			HelHandle copyHandle;
			HEL_CHECK(helForkMemory(area.copyView.getHandle(), &copyHandle));
			copyView = helix::UniqueDescriptor{copyHandle};

			void *pointer;
			HelError error = helMapMemory(copyView.getHandle(), context->_space.getHandle(),
					reinterpret_cast<void *>(address),
					0, area.areaSize, area.nativeFlags, &pointer);
			if(error != kHelErrNone && error != kHelErrAlreadyExists) {
				HEL_CHECK(error);
			}
		}else{
			void *pointer;
			HEL_CHECK(helMapMemory(area.fileView.getHandle(), context->_space.getHandle(),
					reinterpret_cast<void *>(address),
					area.offset, area.areaSize, area.nativeFlags, &pointer));
		}

		Area copy;
		copy.copyOnWrite = area.copyOnWrite;
		copy.areaSize = area.areaSize;
		copy.nativeFlags = area.nativeFlags;
		copy.fileView = area.fileView.dup();
		copy.copyView = std::move(copyView);
		copy.file = area.file;
		copy.offset = area.offset;
		context->_areaTree.emplace(address, std::move(copy));
	}

	return context;
}

VmContext::~VmContext() {
	if(logCleanup_)
		std::cout << "\e[33mposix: VmContext is destructed\e[39m" << std::endl;
}

auto VmContext::splitAreaOn_(uintptr_t addr, size_t size) ->
		std::pair<
			std::map<uintptr_t, Area>::iterator,
			std::map<uintptr_t, Area>::iterator
		> {
	// Avoid accessing out of bounds iterators
	if (!_areaTree.size())
		return {_areaTree.end(), _areaTree.end()};

	auto performSingleSplit = [this] (uintptr_t addr) {
		auto it = _areaTree.upper_bound(addr);
		if (it != _areaTree.begin())
			it = std::prev(it);

		auto &[base, area] = *it;

		if (base < addr && (base + area.areaSize) > addr) {
			Area right;
			right.copyOnWrite = area.copyOnWrite;
			right.areaSize = area.areaSize - (addr - base);
			right.nativeFlags = area.nativeFlags;
			right.fileView = area.fileView.dup();
			right.copyView = area.copyView.dup();
			right.file = area.file;
			right.offset = area.offset + (addr - base);

			_areaTree.emplace(addr, std::move(right));

			area.areaSize = (addr - base);
		}

		return it;
	};

	return {
		performSingleSplit(addr),
		std::next(performSingleSplit(addr + size))
	};
}

async::result<frg::expected<Error, void *>>
VmContext::mapFile(uintptr_t hint, helix::UniqueDescriptor memory,
		smarter::shared_ptr<File, FileHandle> file,
		intptr_t offset, size_t size, bool copyOnWrite, uint32_t nativeFlags) {
	size_t alignedSize = (size + 0xFFF) & ~size_t(0xFFF);

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	helix::UniqueDescriptor copyView;
	void *pointer;
	HelError error;
	if(copyOnWrite) {
		HelHandle handle;
		if(memory) {
			HEL_CHECK(helCopyOnWrite(memory.getHandle(), offset, alignedSize, &handle));
		}else{
			HEL_CHECK(helCopyOnWrite(kHelZeroMemory, offset, alignedSize, &handle));
		}
		copyView = helix::UniqueDescriptor{handle};

		error = helMapMemory(copyView.getHandle(), _space.getHandle(),
				reinterpret_cast<void *>(hint),
				0, alignedSize, nativeFlags, &pointer);
	}else{
		error = helMapMemory(memory.getHandle(), _space.getHandle(),
				reinterpret_cast<void *>(hint),
				offset, alignedSize, nativeFlags, &pointer);
	}

	if(error == kHelErrAlreadyExists) {
		co_return Error::alreadyExists;
	}else if(error == kHelErrNoMemory)
		co_return Error::noMemory;
	HEL_CHECK(error);

	//std::cout << "posix: VM_MAP returns " << pointer
	//		<< " (size: " << (void *)size << ")" << std::endl;

	auto address = reinterpret_cast<uintptr_t>(pointer);

	auto [startIt, endIt] = splitAreaOn_(address, alignedSize);

	for (auto it = startIt; it != endIt;) {
		const auto &[addr, area] = *it;
		if (addr >= address && (addr + area.areaSize) <= (address + alignedSize))
			it = _areaTree.erase(it);
		else
			++it;
	}

	// Construct the new area.
	Area area;
	area.copyOnWrite = copyOnWrite;
	area.areaSize = alignedSize;
	area.nativeFlags = nativeFlags;
	area.fileView = std::move(memory);
	area.copyView = std::move(copyView);
	area.file = std::move(file);
	area.offset = offset;
	_areaTree.emplace(address, std::move(area));

	co_return pointer;
}

async::result<void *> VmContext::remapFile(void *oldPointer,
		size_t oldSize, size_t newSize) {
	size_t alignedOldSize = (oldSize + 0xFFF) & ~size_t(0xFFF);
	size_t alignedNewSize = (newSize + 0xFFF) & ~size_t(0xFFF);

//	std::cout << "posix: Remapping " << oldPointer << std::endl;
	auto it = _areaTree.find(reinterpret_cast<uintptr_t>(oldPointer));
	assert(it != _areaTree.end());
	assert(it->second.areaSize == alignedOldSize);

	assert(!it->second.copyOnWrite);

	auto memory = co_await it->second.file->accessMemory();

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	void *pointer;
	HEL_CHECK(helMapMemory(memory.getHandle(), _space.getHandle(),
			nullptr, it->second.offset, alignedNewSize,
			it->second.nativeFlags, &pointer));
//	std::cout << "posix: VM_REMAP returns " << pointer << std::endl;

	// Unmap the old area.
	HEL_CHECK(helUnmapMemory(_space.getHandle(), oldPointer, alignedOldSize));

	// Construct the new area from the old one.
	Area area;
	area.copyOnWrite = it->second.copyOnWrite;
	area.areaSize = alignedNewSize;
	area.nativeFlags = it->second.nativeFlags;
	area.fileView = std::move(it->second.fileView);
	area.copyView = std::move(it->second.copyView);
	area.file = std::move(it->second.file);
	area.offset = it->second.offset;
	_areaTree.erase(it);

	// Perform some sanity checking.
	auto address = reinterpret_cast<uintptr_t>(pointer);
	auto succ = _areaTree.lower_bound(address + alignedNewSize);
	if(succ != _areaTree.begin()) {
		auto pred = std::prev(succ);
		assert(pred->first + pred->second.areaSize <= address);
	}

	_areaTree.insert({address, std::move(area)});

	co_return pointer;
}

async::result<void> VmContext::protectFile(void *pointer, size_t size, uint32_t protectionFlags) {
	size_t alignedSize = (size + 0xFFF) & ~size_t(0xFFF);
	auto address = reinterpret_cast<uintptr_t>(pointer);

	helix::ProtectMemory protect;
	auto &&submit = helix::submitProtectMemory(_space, &protect,
			pointer, alignedSize, protectionFlags, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(protect.error());

	auto [startIt, endIt] = splitAreaOn_(address, alignedSize);

	for (auto it = startIt; it != endIt; ++it) {
		auto &[addr, area] = *it;
		if (addr >= address && (addr + area.areaSize) <= (address + alignedSize)) {
			area.nativeFlags &= ~(kHelMapProtRead | kHelMapProtWrite | kHelMapProtExecute);
			area.nativeFlags |= protectionFlags;
		}
	}
}

void VmContext::unmapFile(void *pointer, size_t size) {
	size_t alignedSize = (size + 0xFFF) & ~size_t(0xFFF);
	auto address = reinterpret_cast<uintptr_t>(pointer);

	HEL_CHECK(helUnmapMemory(_space.getHandle(), pointer, alignedSize));

	auto [startIt, endIt] = splitAreaOn_(address, alignedSize);

	for (auto it = startIt; it != endIt;) {
		auto &[addr, area] = *it;
		if (addr >= address && (addr + area.areaSize) <= (address + alignedSize)) {
			it = _areaTree.erase(it);
		} else {
			++it;
		}
	}
}

// ----------------------------------------------------------------------------
// FsContext.
// ----------------------------------------------------------------------------

std::shared_ptr<FsContext> FsContext::create() {
	auto context = std::make_shared<FsContext>();

	context->_root = rootPath();
	context->_workDir = rootPath();

	return context;
}

std::shared_ptr<FsContext> FsContext::clone(std::shared_ptr<FsContext> original) {
	auto context = std::make_shared<FsContext>();

	context->_root = original->_root;
	context->_workDir = original->_workDir;
	context->_umask = original->_umask;

	return context;
}

ViewPath FsContext::getRoot() {
	return _root;
}

ViewPath FsContext::getWorkingDirectory() {
	return _workDir;
}

void FsContext::changeRoot(ViewPath root) {
	_root = std::move(root);
}

void FsContext::changeWorkingDirectory(ViewPath workdir) {
	_workDir = std::move(workdir);
}

mode_t FsContext::getUmask() {
	return _umask;
}

mode_t FsContext::setUmask(mode_t mask) {
	mode_t old = _umask;
	_umask = mask & 0777;
	return old;
}

// ----------------------------------------------------------------------------
// FileContext.
// ----------------------------------------------------------------------------

static HelHandle posixMbusClient = [] {
	posix::ManagarmProcessData data;

	HEL_CHECK(helSyscall1(kHelCallSuper + 1, reinterpret_cast<HelWord>(&data)));

	return data.mbusLane;
}();

std::shared_ptr<FileContext> FileContext::create() {
	auto context = std::make_shared<FileContext>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	context->_universe = helix::UniqueDescriptor(universe);

	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &window));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->_fileTableWindow = reinterpret_cast<HelHandle *>(window);

	HEL_CHECK(helTransferDescriptor(
	    posixMbusClient, context->_universe.getHandle(), kHelTransferDescriptorOut, &context->_clientMbusLane
	));

	return context;
}

std::shared_ptr<FileContext> FileContext::clone(std::shared_ptr<FileContext> original) {
	auto context = std::make_shared<FileContext>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	context->_universe = helix::UniqueDescriptor(universe);

	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &window));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->_fileTableWindow = reinterpret_cast<HelHandle *>(window);

	for(auto entry : original->_fileTable) {
		//std::cout << "Clone FD " << entry.first << std::endl;
		context->attachFile(entry.first, entry.second.file, entry.second.closeOnExec);
	}

	HEL_CHECK(helTransferDescriptor(
	    posixMbusClient, context->_universe.getHandle(), kHelTransferDescriptorOut, &context->_clientMbusLane
	));

	return context;
}

FileContext::~FileContext() {
	if(logCleanup_)
		std::cout << "\e[33mposix: FileContext is destructed\e[39m" << std::endl;
}

int FileContext::attachFile(smarter::shared_ptr<File, FileHandle> file,
		bool closeOnExec, int startAt) {
	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(
	    file->getPassthroughLane().getHandle(), _universe.getHandle(), kHelTransferDescriptorOut, &handle
	));

	for(int fd = startAt; ; fd++) {
		if(_fileTable.find(fd) != _fileTable.end())
			continue;

		if(logFileAttach)
			std::cout << "posix: Attaching FD " << fd << std::endl;

		_fileTable.insert({fd, {std::move(file), closeOnExec}});
		_fileTableWindow[fd] = handle;
		return fd;
	}
}

void FileContext::attachFile(int fd, smarter::shared_ptr<File, FileHandle> file,
		bool close_on_exec) {
	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(
	    file->getPassthroughLane().getHandle(), _universe.getHandle(), kHelTransferDescriptorOut, &handle
	));

	if(logFileAttach)
		std::cout << "posix: Attaching fixed FD " << fd << std::endl;

	auto it = _fileTable.find(fd);
	if(it != _fileTable.end()) {
		it->second = {std::move(file), close_on_exec};
	}else{
		_fileTable.insert({fd, {std::move(file), close_on_exec}});
	}
	_fileTableWindow[fd] = handle;
}

std::optional<FileDescriptor> FileContext::getDescriptor(int fd) {
	auto file = _fileTable.find(fd);
	if(file == _fileTable.end())
		return std::nullopt;
	return file->second;
}

Error FileContext::setDescriptor(int fd, bool close_on_exec) {
	auto it = _fileTable.find(fd);
	if(it == _fileTable.end()) {
		return Error::noSuchFile;
	}
	it->second.closeOnExec = close_on_exec;
	return Error::success;
}

smarter::shared_ptr<File, FileHandle> FileContext::getFile(int fd) {
	auto file = _fileTable.find(fd);
	if(file == _fileTable.end())
		return smarter::shared_ptr<File, FileHandle>{};
	return file->second.file;
}

Error FileContext::closeFile(int fd) {
	if(logFileAttach)
		std::cout << "posix: Closing FD " << fd << std::endl;
	auto it = _fileTable.find(fd);
	if(it == _fileTable.end()) {
		return Error::noSuchFile;
	}

	HEL_CHECK(helCloseDescriptor(_universe.getHandle(), _fileTableWindow[fd]));

	_fileTableWindow[fd] = 0;
	_fileTable.erase(it);
	return Error::success;
}

void FileContext::closeOnExec() {
	auto it = _fileTable.begin();
	while(it != _fileTable.end()) {
		if(it->second.closeOnExec) {
			HEL_CHECK(helCloseDescriptor(_universe.getHandle(), _fileTableWindow[it->first]));

			_fileTableWindow[it->first] = 0;
			it = _fileTable.erase(it);
		}else{
			it++;
		}
	}
}

// ----------------------------------------------------------------------------
// SignalContext.
// ----------------------------------------------------------------------------

void CompileSignalInfo::operator() (const UserSignal &info) const {
	//si->si_code = SI_USER;
	si->si_pid = info.pid;
	si->si_uid = info.uid;
}

void CompileSignalInfo::operator() (const TimerSignal &info) const {
	si->si_code = SI_TIMER;
	si->si_timerid = info.timerId;
}

SignalContext::SignalContext()
: _currentSeq{1}, _activeSet{0} { }

std::shared_ptr<SignalContext> SignalContext::create() {
	auto context = std::make_shared<SignalContext>();

	// All signals use their default disposition.
	for(int sn = 1; sn <= 64; sn++)
		context->_handlers[sn - 1].disposition = SignalDisposition::none;

	return context;
}

std::shared_ptr<SignalContext> SignalContext::clone(std::shared_ptr<SignalContext> original) {
	auto context = std::make_shared<SignalContext>();

	// Copy the current signal handler table.
	for(int sn = 1; sn <= 64; sn++)
		context->_handlers[sn - 1] = original->_handlers[sn - 1];

	return context;
}

void SignalContext::resetHandlers() {
	for(int sn = 1; sn <= 64; sn++)
		if(_handlers[sn - 1].disposition == SignalDisposition::handle)
			_handlers[sn - 1].disposition = SignalDisposition::none;
}

SignalHandler SignalContext::getHandler(int sn) {
	return _handlers[sn - 1];
}

SignalHandler SignalContext::changeHandler(int sn, SignalHandler handler) {
	assert(sn - 1 < 64);
	return std::exchange(_handlers[sn - 1], handler);
}

void SignalContext::issueSignal(int sn, SignalInfo info) {
	assert(sn > 0);
	assert(sn - 1 < 64);
	auto item = new SignalItem;
	item->signalNumber = sn;
	item->info = info;

	_slots[sn - 1].raiseSeq = ++_currentSeq;
	_slots[sn - 1].asyncQueue.push_back(*item);
	_activeSet |= (UINT64_C(1) << (sn - 1));
	_signalBell.raise();
}

async::result<PollSignalResult>
SignalContext::pollSignal(uint64_t in_seq, uint64_t mask,
		async::cancellation_token cancellation) {
	assert(in_seq <= _currentSeq);

	while(in_seq == _currentSeq && !cancellation.is_cancellation_requested())
		co_await _signalBell.async_wait(cancellation);

	// Wait until one of the requested signals becomes active.
	while(!(_activeSet & mask) && !cancellation.is_cancellation_requested())
		co_await _signalBell.async_wait(cancellation);

	uint64_t edges = 0;
	for(int sn = 1; sn <= 64; sn++)
		if(_slots[sn - 1].raiseSeq > in_seq)
			edges |= UINT64_C(1) << (sn - 1);

	co_return PollSignalResult{_currentSeq, edges};
}

CheckSignalResult SignalContext::checkSignal() {
	return CheckSignalResult(_currentSeq, _activeSet);
}

async::result<SignalItem *> SignalContext::fetchSignal(uint64_t mask, bool nonBlock, async::cancellation_token ct) {
	int sn;
	while(true) {
		for(sn = 1; sn <= 64; sn++) {
			if(!(mask & (UINT64_C(1) << (sn - 1))))
				continue;
			if(!_slots[sn - 1].asyncQueue.empty())
				break;
		}
		if(sn - 1 != 64)
			break;
		if(nonBlock)
			co_return nullptr;
		if (!co_await _signalBell.async_wait(ct))
			co_return nullptr;
	}

	assert(!_slots[sn - 1].asyncQueue.empty());
	auto item = &_slots[sn - 1].asyncQueue.front();
	_slots[sn - 1].asyncQueue.pop_front();
	if(_slots[sn - 1].asyncQueue.empty())
		_activeSet &= ~(UINT64_C(1) << (sn - 1));

	co_return item;
}

// We follow a similar model as Linux. The linux layout is a follows:
// struct rt_sigframe. Placed at the top of the stack.
//     struct ucontext. Part of struct rt_sigframe.
//         struct sigcontext. Part of struct ucontext.
//             Actually stores the registers.
//             Stores a pointer to the FPU state.
//     siginfo_t. Part of struct rt_sigframe.
// FPU state is store at a higher (undefined) position on the stack.

// This is our signal frame, similar to Linux' struct rt_sigframe.
#if defined(__x86_64__)
struct SignalFrame {
	uintptr_t returnAddress;
	ucontext_t ucontext;
	siginfo_t info;
};
#else
// Return address for 'ret' is stored in X30 and not on the stack
struct SignalFrame {
	ucontext_t ucontext;
	siginfo_t info;
};
#endif

#if defined(__x86_64__)
constexpr size_t redZoneSize = 128;
// Calls misalign the stack by 8 bytes
// We later offset the stack by this amount because the ABI expects
// (rsp + 8) % 16 == 0 at function entry
constexpr size_t stackCallMisalign = 8;
#else
// The AAPCS64 ABI has no red zone
constexpr size_t redZoneSize = 0;
constexpr size_t stackCallMisalign = 0;
#endif

static const auto simdStateSize = [] () -> size_t {
	HelRegisterInfo regInfo;
	HEL_CHECK(helQueryRegisterInfo(kHelRegsSimd, &regInfo));
	return regInfo.setSize;
}();

SignalContext::SignalHandling SignalContext::determineHandling(SignalItem *item, Process *process) {
	SignalHandler handler = _handlers[item->signalNumber - 1];

	process->enterSignal();

	SignalContext::SignalHandling result {
		.handler = handler,
	};

	// Implement SA_RESETHAND by resetting the signal disposition to default.
	if(handler.flags & signalOnce)
		_handlers[item->signalNumber - 1].disposition = SignalDisposition::none;

	// Handle default dispositions properly
	if(handler.disposition == SignalDisposition::none) {
		switch (item->signalNumber) {
			// TODO: Handle SIGTSTP, SIGSTOP and SIGCONT
			case SIGCHLD:
			case SIGURG:
			case SIGWINCH:
				// Ignore the signal.
				result.ignored = true;
				break;
			default:
				result.killed = true;
				break;
		}
	} else if(handler.disposition == SignalDisposition::ignore) {
		// Ignore the signal.
		result.ignored = true;
	} else {
		assert(handler.disposition == SignalDisposition::handle);
	}

	return result;
}

async::result<void> SignalContext::raiseContext(SignalItem *item, Process *process,
		SignalContext::SignalHandling handling) {
	if (handling.ignored) {
		delete item;
		co_return;
	}

	if (handling.handler.disposition == SignalDisposition::none) {
		switch (item->signalNumber) {
			case SIGABRT:
			case SIGILL:
			case SIGSEGV:
				if(debugFaults) {
					std::cout << "posix: Thread " << process->pid() << " killed as the result of signal "
					<< item->signalNumber << std::endl;
					launchGdbServer(process);
					co_await async::suspend_indefinitely(async::cancellation_token{});
				}
				break;
			default:
				std::cout << "posix: Thread " << process->pid() << " killed as the result of signal "
					<< item->signalNumber << std::endl;
				assert(handling.killed);
				break;
		}
	}

	if (handling.killed) {
		co_await process->terminate(TerminationBySignal{item->signalNumber});
		delete item;
		co_return;
	}

	auto thread = process->threadDescriptor();
	SignalFrame sf;
	memset(&sf, 0, sizeof(SignalFrame));

#if defined (__x86_64__)
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));

	sf.returnAddress = handling.handler.restorerIp;
#elif defined(__aarch64__)
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext));
#elif defined(__riscv) && __riscv_xlen == 64
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));
#else
#error Signal register loading code is missing for architecture
#endif

	sf.ucontext.uc_sigmask.sig[0] = process->signalMask();

	sigset_t handlerMask = { process->signalMask() | handling.handler.mask };
	if (!(handling.handler.flags & signalReentrant))
		sigaddset(&handlerMask, item->signalNumber);
	process->setSignalMask(handlerMask.sig[0]);

	std::vector<std::byte> simdState(simdStateSize);
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsSimd, simdState.data()));

	// Once compile siginfo_t if that is neccessary (matches Linux behavior).
	if(handling.handler.flags & signalInfo) {
		sf.info.si_signo = item->signalNumber;
		std::visit(CompileSignalInfo{&sf.info}, item->info);
	}

	// Setup the stack frame.
#if defined(__x86_64__)
	uintptr_t threadSp = sf.ucontext.uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
	uintptr_t threadSp = sf.ucontext.uc_mcontext.sp;
#elif defined(__riscv) && __riscv_xlen == 64
	uintptr_t threadSp = sf.ucontext.uc_mcontext.gregs[REG_SP];
#else
#error Code to get stack pointer is missing for architecture
#endif

	if (handling.handler.flags & signalOnStack && process->isAltStackEnabled()) {
		if (!process->isOnAltStack(threadSp)) {
			threadSp = process->altStackSp() + process->altStackSize();
		}
	}

	uintptr_t nsp = threadSp - redZoneSize;

	auto alignFrame = [&] (size_t size) -> uintptr_t {
		nsp = ((nsp - size) & ~uintptr_t(15)) - stackCallMisalign;
		return nsp;
	};

	size_t totalFrameSize = simdStateSize + sizeof(SignalFrame);

	// Store the current register stack on the stack.
	auto frame = alignFrame(totalFrameSize);
	assert((frame & (alignof(SignalFrame) - 1)) == 0);

#if defined(__x86_64__)
	sf.ucontext.uc_mcontext.fpregs = (struct _fpstate*)(frame + sizeof(SignalFrame));
#endif
	// TODO: aarch64

	auto storeFrame = co_await helix_ng::writeMemory(thread, frame,
			sizeof(SignalFrame), &sf);
	auto storeSimd = co_await helix_ng::writeMemory(thread, frame + sizeof(SignalFrame),
			simdStateSize, simdState.data());
	HEL_CHECK(storeFrame.error());
	HEL_CHECK(storeSimd.error());

	if(logSignals) {
		std::cout << "posix: Saving pre-signal stack to " << (void *)frame << std::endl;
		std::cout << "posix: Calling signal handler at " << (void *)handling.handler.handlerIp << std::endl;
	}
	// Setup the new register image and resume.
#if defined(__x86_64__)
	sf.ucontext.uc_mcontext.gregs[REG_RDI] = item->signalNumber;
	sf.ucontext.uc_mcontext.gregs[REG_RSI] = frame + offsetof(SignalFrame, info);
	sf.ucontext.uc_mcontext.gregs[REG_RDX] = frame + offsetof(SignalFrame, ucontext);
	sf.ucontext.uc_mcontext.gregs[REG_RAX] = 0; // Number of variable arguments.

	sf.ucontext.uc_mcontext.gregs[REG_RIP] = handling.handler.handlerIp;
	sf.ucontext.uc_mcontext.gregs[REG_RSP] = frame;

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));
#elif defined(__aarch64__)
	sf.ucontext.uc_mcontext.regs[0] = item->signalNumber;
	sf.ucontext.uc_mcontext.regs[1] = frame + offsetof(SignalFrame, info);
	sf.ucontext.uc_mcontext.regs[2] = frame + offsetof(SignalFrame, ucontext);

	// Return address for the 'ret' instruction
	sf.ucontext.uc_mcontext.regs[30] = handling.handler.restorerIp;

	sf.ucontext.uc_mcontext.pc = handling.handler.handlerIp;
	sf.ucontext.uc_mcontext.sp = frame;

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext));
#elif defined(__riscv) && __riscv_xlen == 64
	sf.ucontext.uc_mcontext.gregs[REG_A0 + 0] = item->signalNumber;
	sf.ucontext.uc_mcontext.gregs[REG_A0 + 1] = frame + offsetof(SignalFrame, info);
	sf.ucontext.uc_mcontext.gregs[REG_A0 + 2] = frame + offsetof(SignalFrame, ucontext);

	sf.ucontext.uc_mcontext.gregs[REG_RA] = handling.handler.restorerIp;
	sf.ucontext.uc_mcontext.gregs[REG_PC] = handling.handler.handlerIp;
	sf.ucontext.uc_mcontext.gregs[REG_SP] = frame;

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));
#else
#error Signal frame register setup code is missing for architecture
#endif

	delete item;
}

async::result<void> SignalContext::determineAndRaiseContext(SignalItem *item, Process *process,
		bool &killed) {
	auto handling = determineHandling(item, process);
	killed = handling.killed;
	co_return co_await raiseContext(item, process, handling);
}

async::result<void> SignalContext::restoreContext(helix::BorrowedDescriptor thread, Process *process) {
	uintptr_t pcrs[2];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));
	auto frame = pcrs[kHelRegSp] - stackCallMisalign;

	if(logSignals)
		std::cout << "posix: Restoring post-signal stack from " << (void *)frame << std::endl;

	std::vector<std::byte> simdState(simdStateSize);

	SignalFrame sf;
	auto loadFrame = co_await helix_ng::readMemory(thread, frame,
			sizeof(SignalFrame), &sf);
	auto loadSimd = co_await helix_ng::readMemory(thread, frame + sizeof(SignalFrame),
			simdStateSize, simdState.data());
	HEL_CHECK(loadFrame.error());
	HEL_CHECK(loadSimd.error());

	process->setSignalMask(sf.ucontext.uc_sigmask.sig[0]);

#if defined(__x86_64__)
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));
#elif defined(__aarch64__)
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext));
#elif defined(__riscv) && __riscv_xlen == 64
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSignal, &sf.ucontext.uc_mcontext.gregs));
#else
#error Signal register storing code is missing for architecture
#endif

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsSimd, simdState.data()));
}

// ----------------------------------------------------------------------------
// Generation.
// ----------------------------------------------------------------------------

Generation::~Generation() {
	if(logCleanup_)
		std::cout << "\e[33mposix: Generation is destructed\e[39m" << std::endl;
}

// ----------------------------------------------------------------------------
// Process.
// ----------------------------------------------------------------------------

// PID 1 is reserved for the init process, therefore we start at 2.
ProcessId nextPid = 2;
std::map<ProcessId, PidHull *> globalPidMap;

PidHull::PidHull(pid_t pid)
: pid_{pid} {
	auto [it, success] = globalPidMap.insert({pid_, this});
	assert(success);
	(void)it;
}

PidHull::~PidHull() {
	auto it = globalPidMap.find(pid_);
	assert(it != globalPidMap.end());
	globalPidMap.erase(it);
}

void PidHull::initializeProcess(Process *process) {
	process_ = process->weak_from_this();
}

void PidHull::initializeTerminalSession(TerminalSession *session) {
	// TODO: verify that no terminal session is associated with this PidHull.
	terminalSession_ = session->weak_from_this();
}

void PidHull::initializeProcessGroup(ProcessGroup *group) {
	// TODO: verify that no process group is associated with this PidHull.
	processGroup_ = group->weak_from_this();
}

std::shared_ptr<Process> PidHull::getProcess() {
	return process_.lock();
}

std::shared_ptr<ProcessGroup> PidHull::getProcessGroup() {
	return processGroup_.lock();
}

std::shared_ptr<TerminalSession> PidHull::getTerminalSession() {
	return terminalSession_.lock();
}

std::shared_ptr<Process> Process::findProcess(ProcessId pid) {
	auto it = globalPidMap.find(pid);
	if(it == globalPidMap.end())
		return nullptr;
	return it->second->getProcess();
}

Process::Process(std::shared_ptr<PidHull> hull, Process *parent)
: _parent{parent}, _hull{std::move(hull)},
		_clientPosixLane{kHelNullHandle}, _clientFileTable{nullptr},
		notifyType_{NotifyType::null} { }

Process::~Process() {
	std::cout << std::format("\e[33mposix: Process {} is destructed\e[39m", pid()) << std::endl;
	_pgPointer->dropProcess(this);
}

void Process::cancelEvent() {
	auto cancelEventPtr = reinterpret_cast<HelHandle *>(_cancelEventMapping.get());
	auto cancelEvent = __atomic_load_n(cancelEventPtr, __ATOMIC_ACQUIRE);
	if (cancelEvent != kHelNullHandle) {
		HelHandle posixCancelEvent;
		HEL_CHECK(helTransferDescriptor(
		    cancelEvent, _fileContext->getUniverse().getHandle(), kHelTransferDescriptorIn, &posixCancelEvent
		));
		HEL_CHECK(helRaiseEvent(posixCancelEvent));
		*cancelEventPtr = kHelNullHandle;
	}
}

bool Process::checkSignalRaise() {
	auto p = reinterpret_cast<unsigned int *>(accessThreadPage());
	unsigned int gsf = __atomic_load_n(p, __ATOMIC_RELAXED);
	if(!gsf)
		return true;
	return false;
}

bool Process::checkOrRequestSignalRaise() {
	auto p = reinterpret_cast<unsigned int *>(accessThreadPage());
	unsigned int gsf = __atomic_load_n(p, __ATOMIC_RELAXED);
	if(!gsf)
		return true;
	if(gsf == 1) {
		__atomic_store_n(p, 2, __ATOMIC_RELAXED);
	}else if(gsf != 2) {
		std::cout << "\e[33m" "posix: Ignoring unexpected value "
				<< gsf << " of global signal flag" "\e[39m" << std::endl;
	}
	return false;
}

async::result<std::shared_ptr<Process>> Process::init(std::string path) {
	auto hull = std::make_shared<PidHull>(1);
	auto process = std::make_shared<Process>(std::move(hull), nullptr);
	size_t pos = path.rfind('/');
	assert(pos != std::string::npos);
	process->_path = path;
	process->_name = path.substr(pos + 1);
	process->_vmContext = VmContext::create();
	process->_fsContext = FsContext::create();
	process->_fileContext = FileContext::create();
	process->_signalContext = SignalContext::create();

	TerminalSession::initializeNewSession(process.get());

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};

	HelHandle cancel_event_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &cancel_event_memory));
	process->_cancelEventMemory = helix::UniqueDescriptor{cancel_event_memory};
	process->_cancelEventMapping = helix::Mapping{process->_cancelEventMemory, 0, 0x1000};
	new (process->_cancelEventMapping.get()) HelHandle{};

	// The initial signal mask allows all signals.
	process->_signalMask = 0;

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
	    &process->_clientPosixLane
	));
	client_lane.release();

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&process->_clientThreadPage));
	HEL_CHECK(helMapMemory(process->_cancelEventMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&process->_clientCancelEvent));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientFileTable));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientClkTrackerPage));

	process->_uid = 0;
	process->_euid = 0;
	process->_gid = 0;
	process->_egid = 0;
	process->_hull->initializeProcess(process.get());

	// TODO: Do not pass an empty argument vector?
	auto execOutcome = co_await execute(process->_fsContext->getRoot(),
			process->_fsContext->getWorkingDirectory(),
			path, std::vector<std::string>{}, std::vector<std::string>{},
			process->_vmContext,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane(), process.get());
	if(!execOutcome)
		throw std::logic_error("Could not execute() init process");
	auto &execResult = execOutcome.value();

	process->_threadDescriptor = std::move(execResult.thread);
	process->_clientAuxBegin = execResult.auxBegin;
	process->_clientAuxEnd = execResult.auxEnd;
	process->_posixLane = std::move(server_lane);
	process->_didExecute = true;

	auto procfs_root = std::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	process->_procfs_dir = procfs_root->createProcDirectory(std::to_string(process->_hull->getPid()), process.get());

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	helResume(process->_threadDescriptor.getHandle());
	async::detach(serve(process, std::move(generation)));

	co_return process;
}

std::shared_ptr<Process> Process::fork(std::shared_ptr<Process> original) {
	auto hull = std::make_shared<PidHull>(nextPid++);
	auto process = std::make_shared<Process>(std::move(hull), original.get());
	process->_path = original->path();
	process->_name = original->name();
	process->_vmContext = VmContext::clone(original->_vmContext);
	process->_fsContext = FsContext::clone(original->_fsContext);
	process->_fileContext = FileContext::clone(original->_fileContext);
	process->_signalContext = SignalContext::clone(original->_signalContext);

	original->_pgPointer->reassociateProcess(process.get());

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};

	HelHandle cancel_event_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &cancel_event_memory));
	process->_cancelEventMemory = helix::UniqueDescriptor{cancel_event_memory};
	process->_cancelEventMapping = helix::Mapping{process->_cancelEventMemory, 0, 0x1000};
	new (process->_cancelEventMapping.get()) HelHandle{};

	// Signal masks are copied on fork().
	process->_signalMask = original->_signalMask;

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
	    &process->_clientPosixLane
	));
	client_lane.release();

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&process->_clientThreadPage));
	HEL_CHECK(helMapMemory(process->_cancelEventMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&process->_clientCancelEvent));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientFileTable));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientClkTrackerPage));

	process->_clientAuxBegin = original->_clientAuxBegin;
	process->_clientAuxEnd = original->_clientAuxEnd;
	process->_uid = original->_uid;
	process->_euid = original->_euid;
	process->_gid = original->_gid;
	process->_egid = original->_egid;
	original->_children.push_back(process);
	process->_hull->initializeProcess(process.get());
	process->_didExecute = false;

	auto procfs_root = std::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	process->_procfs_dir = procfs_root->createProcDirectory(std::to_string(process->_hull->getPid()), process.get());

	HelHandle new_thread;
	HEL_CHECK(helCreateThread(process->fileContext()->getUniverse().getHandle(),
			process->vmContext()->getSpace().getHandle(), kHelAbiSystemV,
			nullptr, nullptr, kHelThreadStopped, &new_thread));
	process->_threadDescriptor = helix::UniqueDescriptor{new_thread};
	process->_posixLane = std::move(server_lane);

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	async::detach(serve(process, std::move(generation)));

	return process;
}

std::shared_ptr<Process> Process::clone(std::shared_ptr<Process> original, void *ip, void *sp) {
	auto hull = std::make_shared<PidHull>(nextPid++);
	auto process = std::make_shared<Process>(std::move(hull), original.get());
	process->_path = original->path();
	process->_name = original->name();
	process->_vmContext = original->_vmContext;
	process->_fsContext = original->_fsContext;
	process->_fileContext = original->_fileContext;
	process->_signalContext = original->_signalContext;

	// TODO: ProcessGroups should probably store ThreadGroups and not processes.
	original->_pgPointer->reassociateProcess(process.get());

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};

	HelHandle cancel_event_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &cancel_event_memory));
	process->_cancelEventMemory = helix::UniqueDescriptor{cancel_event_memory};
	process->_cancelEventMapping = helix::Mapping{process->_cancelEventMemory, 0, 0x1000};
	new (process->_cancelEventMapping.get()) HelHandle{};

	// Signal masks are copied on clone().
	process->_signalMask = original->_signalMask;

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
	    &process->_clientPosixLane
	));
	client_lane.release();

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&process->_clientThreadPage));
	HEL_CHECK(helMapMemory(process->_cancelEventMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&process->_clientCancelEvent));

	process->_clientFileTable = original->_clientFileTable;
	process->_clientClkTrackerPage = original->_clientClkTrackerPage;

	process->_clientAuxBegin = original->_clientAuxBegin;
	process->_clientAuxEnd = original->_clientAuxEnd;
	process->_uid = original->_uid;
	process->_euid = original->_euid;
	process->_gid = original->_gid;
	process->_egid = original->_egid;
	original->_children.push_back(process);
	process->_hull->initializeProcess(process.get());
	process->_didExecute = false;

	auto procfs_root = std::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	process->_procfs_dir = procfs_root->createProcDirectory(std::to_string(process->_hull->getPid()), process.get());

	HelHandle new_thread;
	HEL_CHECK(helCreateThread(process->fileContext()->getUniverse().getHandle(),
			process->vmContext()->getSpace().getHandle(), kHelAbiSystemV,
			ip, sp, kHelThreadStopped, &new_thread));
	process->_threadDescriptor = helix::UniqueDescriptor{new_thread};
	process->_posixLane = std::move(server_lane);

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	async::detach(serve(process, std::move(generation)));

	return process;
}

async::result<Error> Process::exec(std::shared_ptr<Process> process,
		std::string path, std::vector<std::string> args, std::vector<std::string> env) {
	auto exec_vm_context = VmContext::create();

	// Perform the exec() in a new VM context so that we
	// can catch errors before trashing the calling process.
	auto execResult = FRG_CO_TRY(co_await execute(process->_fsContext->getRoot(),
			process->_fsContext->getWorkingDirectory(),
			path, std::move(args), std::move(env), exec_vm_context,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane(), process.get()));

	// Allocate resources.
	HelHandle exec_posix_lane;
	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(
	    client_lane.getHandle(),
	    process->_fileContext->getUniverse().getHandle(),
	    kHelTransferDescriptorOut,
	    &exec_posix_lane
	));
	client_lane.release();

	void *exec_thread_page;
	void *exec_cancel_event;
	void *exec_clk_tracker_page;
	void *exec_client_table;
	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&exec_thread_page));
	HEL_CHECK(helMapMemory(process->_cancelEventMemory.getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&exec_cancel_event));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&exec_clk_tracker_page));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&exec_client_table));

	// Kill the old thread.
	// After this is done, we cannot roll back the exec() operation.
	HEL_CHECK(helKillThread(process->_threadDescriptor.getHandle()));
	auto previousGeneration = process->_currentGeneration;
	previousGeneration->inTermination = true;
	previousGeneration->cancelServe.cancel();
	co_await previousGeneration->signalsDone.wait();
	co_await previousGeneration->requestsDone.wait();

	// Perform pre-exec() work.
	// From here on, we can now release resources of the old process image.
	process->_fileContext->closeOnExec();

	// "Commit" the exec() operation.
	size_t pos = path.rfind('/');
	assert(pos != std::string::npos);
	process->_name = path.substr(pos + 1);
	process->_path = std::move(path);
	process->_posixLane = std::move(server_lane);
	process->_threadDescriptor = std::move(execResult.thread);
	process->_vmContext = std::move(exec_vm_context);
	process->_signalContext->resetHandlers();
	process->_clientThreadPage = exec_thread_page;
	process->_clientCancelEvent = exec_cancel_event;
	process->_clientPosixLane = exec_posix_lane;
	process->_clientFileTable = exec_client_table;
	process->_clientClkTrackerPage = exec_clk_tracker_page;
	process->_clientAuxBegin = execResult.auxBegin;
	process->_clientAuxEnd = execResult.auxEnd;
	process->_didExecute = true;

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	helResume(process->_threadDescriptor.getHandle());
	async::detach(serve(process, std::move(generation)));

	co_return Error::success;
}

void Process::retire(Process *process) {
	if(process->_procfs_dir)
		process->_procfs_dir->unlinkSelf();

	assert(process->_parent);
	process->_parent->_childrenUsage.userTime += process->_generationUsage.userTime;

	std::erase_if(process->_parent->_children, [process](auto e) {
		return e.get() == process;
	});
}

async::result<void> Process::terminate(TerminationState state) {
	auto parent = getParent();
	assert(parent);

	// Kill the current thread and accumulate stats.
	HEL_CHECK(helKillThread(_threadDescriptor.getHandle()));
	_currentGeneration->inTermination = true;
	_currentGeneration->cancelServe.cancel();
	co_await _currentGeneration->signalsDone.wait();
	co_await _currentGeneration->requestsDone.wait();

	// TODO: Also do this before switching to a new Generation in execve().
	// TODO: Do the accumulation + _currentGeneration reset after the thread has really terminated?
	HelThreadStats stats;
	HEL_CHECK(helQueryThreadStats(_threadDescriptor.getHandle(), &stats));
	_generationUsage.userTime += stats.userTime;

	if(realTimer)
		realTimer->cancel();

	_posixLane = {};
	_threadDescriptor = {};
	_vmContext = nullptr;
	_fsContext = nullptr;
	_fileContext = nullptr;
	//_signalContext = nullptr; // TODO: Migrate the notifications to PID 1.
	_currentGeneration = nullptr;

	auto reparent_to = parent;
	// walk up the chain until we hit a process that has no parent
	while(reparent_to->getParent())
		reparent_to = reparent_to->getParent();

	for(auto it = _children.begin(); it != _children.end();) {
		(*it)->_parent = reparent_to;
		reparent_to->_children.push_back((*it));

		// send the signal if it requested one on parent death
		if((*it)->parentDeathSignal_) {
			UserSignal info;
			info.pid = pid();
			(*it)->signalContext()->issueSignal((*it)->parentDeathSignal_.value(), info);
		}

		it = _children.erase(it);
	}

	// Notify the parent of our status change.
	assert(notifyType_ == NotifyType::null);
	notifyType_ = NotifyType::terminated;
	_state = std::move(state);
	notifyTypeChange_.raise();

	UserSignal info;
	info.pid = pid();

	auto sigchldHandling = parent->signalContext()->getHandler(SIGCHLD);
	if (sigchldHandling.disposition != SignalDisposition::ignore && !(sigchldHandling.flags & signalNoChildWait))
		parent->_notifyQueue.push_back(*this);
	else
		Process::retire(this);

	parent->_notifyBell.raise();

	// Send SIGCHLD to the parent.
	parent->signalContext()->issueSignal(SIGCHLD, info);
}

async::result<frg::expected<Error, Process::WaitResult>>
Process::wait(int pid, WaitFlags flags, async::cancellation_token ct) {
	assert(pid == -1 || pid > 0);
	assert(flags & waitExited);
	assert(!(flags & ~(waitNonBlocking | waitExited | waitLeaveZombie)));

	if(_children.empty() || (pid > 0 && !hasChild(pid)))
		co_return Error::noChildProcesses;

	std::optional<WaitResult> result{};
	while(true) {
		for(auto it = _notifyQueue.begin(); it != _notifyQueue.end(); ++it) {
			if(pid > 0 && pid != it->pid())
				continue;
			if(std::holds_alternative<TerminationByExit>(it->_state) && !(flags & (waitExited)))
				continue;
			if(std::holds_alternative<TerminationBySignal>(it->_state) && !(flags & (waitExited)))
				continue;

			result = WaitResult{
				.pid = it->pid(),
				.uid = it->uid(),
				.state = it->_state,
				.stats = it->_generationUsage,
			};

			if(!(flags & waitLeaveZombie)) {
				// erasing from the queue invalidates the iterator, so we take a pointer before
				auto proc = &(*it);
				_notifyQueue.erase(it);
				Process::retire(proc);
			}

			break;
		}

		if(result || (flags & waitNonBlocking))
			co_return result.value_or(WaitResult{});

		if (!co_await _notifyBell.async_wait(ct))
			co_return Error::interrupted;

		if (_children.empty())
			co_return Error::noChildProcesses;
	}
}

bool Process::hasChild(int pid) {
	return std::ranges::find_if(_children, [pid](auto e) {
		return e->pid() == pid;
	}) != _children.end();
}

async::result<bool> Process::awaitNotifyTypeChange(async::cancellation_token token) {
	co_return co_await notifyTypeChange_.async_wait(token);
}

// --------------------------------------------------------------------------------------
// Process groups and sessions.
// --------------------------------------------------------------------------------------

std::shared_ptr<ProcessGroup> ProcessGroup::findProcessGroup(ProcessId pid) {
	auto it = globalPidMap.find(pid);
	if(it == globalPidMap.end())
		return nullptr;
	return it->second->getProcessGroup();
}

ProcessGroup::ProcessGroup(std::shared_ptr<PidHull> hull)
: hull_{std::move(hull)} { }

ProcessGroup::~ProcessGroup() {
	sessionPointer_->dropGroup(this);
}

void ProcessGroup::reassociateProcess(Process *process) {
	if(process->_pgPointer) {
		auto oldGroup = process->_pgPointer.get();
		oldGroup->members_.erase(oldGroup->members_.iterator_to(*process));
	}
	process->_pgPointer = shared_from_this();
	members_.push_back(*process);
}

void ProcessGroup::dropProcess(Process *process) {
	assert(process->_pgPointer.get() == this);
	members_.erase(members_.iterator_to(*process));
	// Note: this assignment can destruct 'this'.
	process->_pgPointer = nullptr;
}

void ProcessGroup::issueSignalToGroup(int sn, SignalInfo info) {
	for(auto &processRef : members_)
		processRef.signalContext()->issueSignal(sn, info);
}

TerminalSession::TerminalSession(std::shared_ptr<PidHull> hull)
: hull_{std::move(hull)} { }

TerminalSession::~TerminalSession() {
	if(ctsPointer_)
		ctsPointer_->dropSession(this);
}

pid_t TerminalSession::getSessionId() {
	return hull_->getPid();
}

std::shared_ptr<TerminalSession> TerminalSession::initializeNewSession(Process *sessionLeader) {
	auto session = std::make_shared<TerminalSession>(sessionLeader->getHull()->shared_from_this());
	auto group = session->spawnProcessGroup(sessionLeader);
	session->foregroundGroup_ = group.get();
	session->hull_->initializeTerminalSession(session.get());
	return session;
}

std::shared_ptr<ProcessGroup> TerminalSession::spawnProcessGroup(Process *groupLeader) {
	auto group = std::make_shared<ProcessGroup>(groupLeader->getHull()->shared_from_this());
	group->reassociateProcess(groupLeader);
	group->sessionPointer_ = shared_from_this();
	groups_.push_back(*group);
	group->hull_->initializeProcessGroup(group.get());
	return group;
}

std::shared_ptr<ProcessGroup> TerminalSession::getProcessGroupById(pid_t id) {
	for(auto &i : groups_) {
		if(i.getHull()->getPid() == id)
			return i.getHull()->getProcessGroup()->shared_from_this();
	}
	return nullptr;
}

void TerminalSession::dropGroup(ProcessGroup *group) {
	assert(group->sessionPointer_.get() == this);
	if(foregroundGroup_ == group)
		foregroundGroup_ = nullptr;
	groups_.erase(groups_.iterator_to(*group));
	// Note: this assignment can destruct 'this'.
	group->sessionPointer_ = nullptr;
}

Error TerminalSession::setForegroundGroup(ProcessGroup *group) {
	assert(group);
	if(group->sessionPointer_.get() != this)
		return Error::insufficientPermissions;
	foregroundGroup_ = group;
	return Error::success;
}

Error ControllingTerminalState::assignSessionOf(Process *process) {
	auto group = process->_pgPointer.get();
	auto session = group->sessionPointer_.get();
	if(process->getHull() != session->hull_.get())
		return Error::illegalArguments; // Process is not a session leader.
	if(associatedSession_)
		return Error::insufficientPermissions;
	if(session->ctsPointer_)
		return Error::insufficientPermissions;
	associatedSession_ = session;
	session->ctsPointer_ = this;
	return Error::success;
}

void ControllingTerminalState::dropSession(TerminalSession *session) {
	assert(associatedSession_ == session);
	associatedSession_ = nullptr;
	session->ctsPointer_ = nullptr;
}

void ControllingTerminalState::issueSignalToForegroundGroup(int sn, SignalInfo info) {
	if(!associatedSession_)
		return;
	if(!associatedSession_->foregroundGroup_)
		return;
	associatedSession_->foregroundGroup_->issueSignalToGroup(sn, info);
}
