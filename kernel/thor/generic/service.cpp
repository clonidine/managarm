#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/span.hpp>
#include <frg/string.hpp>
#include <fs.frigg_bragi.hpp>
#include <posix.frigg_bragi.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/gdbserver.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/module.hpp>
#include <thor-internal/servers.hpp>
#include <thor-internal/stream.hpp>
#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>

#define __MLIBC_ABI_ONLY
#include <sys/mman.h>
#undef __MLIBC_ABI_ONLY

namespace thor {

extern frg::manual_box<LaneHandle> mbusClient;

void runService(frg::string<KernelAlloc> name, LaneHandle controlLane, smarter::shared_ptr<Thread, ActiveHandle> thread);

struct OpenFile {
	OpenFile()
	: isTerminal{false} { };

	bool isTerminal;
	LaneHandle clientLane;
};

struct StdioFile : OpenFile {
	StdioFile() {
		isTerminal = true;
	}
};

namespace stdio {
	coroutine<void> runStdioRequests(LaneHandle lane) {
		frg::string<KernelAlloc> lineBuffer{*kernelAlloc};

		while(true) {
			auto [acceptError, conversation] = co_await AcceptSender{lane};
			if(acceptError == Error::endOfLane)
				break;
			if(acceptError != Error::success) {
				infoLogger() << "thor: Could not accept stdio lane" << frg::endlog;
				co_return;
			}
			auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
			if(reqError != Error::success) {
				infoLogger() << "thor: Could not receive stdio request" << frg::endlog;
				co_return;
			}

			managarm::fs::CntRequest<KernelAlloc> req(*kernelAlloc);
			req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

			if(req.req_type() == managarm::fs::CntReqType::WRITE) {
				auto [credsError, credentials] = co_await ExtractCredentialsSender{conversation};
				if(credsError != Error::success) {
					infoLogger() << "thor: Could not receive stdio credentials"
							<< frg::endlog;
					co_return;
				}
				auto [dataError, dataBuffer] = co_await RecvBufferSender{conversation};
				if(dataError != Error::success) {
					infoLogger() << "thor: Could not receive stdio data" << frg::endlog;
					co_return;
				}

				for(size_t i = 0; i < dataBuffer.size(); i++) {
					auto c = reinterpret_cast<char *>(dataBuffer.data())[i];
					if(c == '\n') {
						infoLogger() << lineBuffer << frg::endlog;
						lineBuffer.resize(0);
					}else{
						lineBuffer += c;
					}
				}

				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_size(dataBuffer.size());

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(req.req_type() == managarm::fs::CntReqType::SEEK_REL) {
				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SEEK_ON_PIPE);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else{
				urgentLogger() << "thor: Illegal request type " << (int32_t)req.req_type()
						<< " for kernel provided stdio file" << frg::endlog;

				auto dismissError = co_await DismissSender{conversation};
				// TODO: improve error handling here.
				assert(dismissError == Error::success);
			}
		}
	}

} // namespace stdio

namespace initrd {
	struct OpenRegular : OpenFile {
		OpenRegular(MfsRegular *module)
		: module(module), offset(0) { }

		MfsRegular *module;
		size_t offset;
	};

	struct OpenDirectory : OpenFile {
		OpenDirectory(MfsDirectory *node)
		: node{node}, index(0) { }

		MfsDirectory *node;
		size_t index;
	};

	// ----------------------------------------------------
	// initrd file handling.
	// ----------------------------------------------------

	coroutine<void> runRegularRequests(OpenRegular *file, LaneHandle lane) {
		while(true) {
			auto [acceptError, conversation] = co_await AcceptSender{lane};
			if(acceptError == Error::endOfLane)
				break;
			if(acceptError != Error::success) {
				infoLogger() << "thor: Could not accept regular lane" << frg::endlog;
				co_return;
			}
			auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
			if(reqError != Error::success) {
				infoLogger() << "thor: Could not receive regular request" << frg::endlog;
				co_return;
			}

			managarm::fs::CntRequest<KernelAlloc> req(*kernelAlloc);
			req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

			if(req.req_type() == managarm::fs::CntReqType::READ) {
				//TODO(geert): Maybe use this event to cancel
				// stuff as well?
				auto [eventError, event] = co_await PullDescriptorSender{conversation};
				if (eventError != Error::success) {
					infoLogger() << "thor: Could not receive read event"
							<< frg::endlog;
					co_return;
				}

				auto [credsError, credentials] = co_await ExtractCredentialsSender{conversation};
				if(credsError != Error::success) {
					infoLogger() << "thor: Could not receive stdio credentials"
							<< frg::endlog;
					co_return;
				}

				frg::unique_memory<KernelAlloc> dataBuffer{*kernelAlloc,
						frg::min(size_t(req.size()), file->module->size() - file->offset)};
				auto copyOutcome = co_await file->module->getMemory()->copyFrom(file->offset,
					dataBuffer.data(), dataBuffer.size(), WorkQueue::generalQueue()->take());
				assert(copyOutcome);
				file->offset += dataBuffer.size();

				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);

				auto dataError = co_await SendBufferSender{conversation, std::move(dataBuffer)};
				// TODO: improve error handling here.
				assert(dataError == Error::success);
			}else if(req.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
				file->offset = req.rel_offset();

				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(req.req_type() == managarm::fs::CntReqType::MMAP) {
				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);

				auto memoryError = co_await PushDescriptorSender{conversation,
						MemoryViewDescriptor{file->module->getMemory()}};
				// TODO: improve error handling here.
				assert(memoryError == Error::success);
			}else{
				urgentLogger() << "thor: Illegal request type " << (int32_t)req.req_type()
						<< " for kernel provided regular file" << frg::endlog;

				auto dismissError = co_await DismissSender{conversation};
				// TODO: improve error handling here.
				assert(dismissError == Error::success);
			}

		}
	}

	coroutine<void> runDirectoryRequests(OpenDirectory *file, LaneHandle lane) {
		while(true) {
			auto [acceptError, conversation] = co_await AcceptSender{lane};
			if(acceptError == Error::endOfLane)
				break;
			if(acceptError != Error::success) {
				infoLogger() << "thor: Could not accept directory lane" << frg::endlog;
				co_return;
			}
			auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
			if(reqError != Error::success) {
				infoLogger() << "thor: Could not receive directory request" << frg::endlog;
				co_return;
			}

			auto preamble = bragi::read_preamble(reqBuffer);
			if(preamble.error()) {
				infoLogger() << "thor: Could not decode directory request preamble" << frg::endlog;
				co_return;
			}

			if(preamble.id() == managarm::fs::ReadEntriesRequest<KernelAlloc>::message_id) {
				if(file->index < file->node->numEntries()) {
					auto entry = file->node->getEntry(file->index);

					managarm::fs::ReadEntriesResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::fs::Errors::SUCCESS);
					resp.set_path(entry.name);
					if(entry.node->type == MfsType::directory) {
						resp.set_file_type(managarm::fs::FileType::DIRECTORY);
					}else{
						assert(entry.node->type == MfsType::regular);
						resp.set_file_type(managarm::fs::FileType::REGULAR);
					}

					file->index++;

					frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc,
						resp.head_size};
					frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc,
						resp.size_of_tail()};

					bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);

					auto respHeadError = co_await SendBufferSender{conversation, std::move(respHeadBuffer)};
					if (respHeadError != Error::success) {
						infoLogger() << "thor: Could not send ReadEntriesResponse head" << frg::endlog;
						co_return;
					}

					auto respTailError = co_await SendBufferSender{conversation, std::move(respTailBuffer)};
					if (respTailError != Error::success) {
						infoLogger() << "thor: Could not send ReadEntriesResponse tail" << frg::endlog;
						co_return;
					}
				}else{
					managarm::fs::ReadEntriesResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::fs::Errors::END_OF_FILE);

					frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc,
						resp.head_size};
					frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc,
						resp.size_of_tail()};

					bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);

					auto respHeadError = co_await SendBufferSender{conversation, std::move(respHeadBuffer)};
					if (respHeadError != Error::success) {
						infoLogger() << "thor: Could not send ReadEntriesResponse head" << frg::endlog;
						co_return;
					}

					auto respTailError = co_await SendBufferSender{conversation, std::move(respTailBuffer)};
					if (respTailError != Error::success) {
						infoLogger() << "thor: Could not send ReadEntriesResponse tail" << frg::endlog;
						co_return;
					}
				}
			}else{
				urgentLogger() << "thor: Illegal request with message ID " << preamble.id()
						<< " for kernel provided directory file" << frg::endlog;

				auto dismissError = co_await DismissSender{conversation};
				// TODO: improve error handling here.
				assert(dismissError == Error::success);
			}
		}
	}
} // namepace initrd

namespace posix {
	struct ThreadInfo {
		smarter::shared_ptr<Thread, ActiveHandle> thread;
		uint64_t tid;
		Handle posixHandle;
	};

	// ----------------------------------------------------
	// POSIX server.
	// ----------------------------------------------------

	struct Process {
		Process(frg::string<KernelAlloc> name)
		: _name{std::move(name)}, openFiles(*kernelAlloc) {
			fileTableMemory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, 0x1000);
			fileTableMemory->selfPtr = fileTableMemory;
		}

		coroutine<void> setupAddressSpace(smarter::shared_ptr<Thread, ActiveHandle> thread) {
			auto view = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
					fileTableMemory, 0, 0x1000);
			auto result = co_await thread->getAddressSpace()->map(std::move(view),
					0, 0, 0x1000,
					AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead);
			assert(result);
			clientFileTable = result.value();
		}

		coroutine<void> runPosixRequests(ThreadInfo info, LaneHandle posixLane);
		coroutine<void> runObserveLoop(ThreadInfo info);

		frg::string_view name() {
			return _name;
		}

		ThreadInfo &attachThread(smarter::shared_ptr<Thread, ActiveHandle> thread) {
			auto posixStream = createStream();
			Handle posixHandle;

			{
				auto irqLock = frg::guard(&irqMutex());
				Universe::Guard universeLock(thread->getUniverse()->lock);

				posixHandle = thread->getUniverse()->attachDescriptor(universeLock,
					LaneDescriptor{std::move(posixStream.get<1>())});
			}

			ThreadInfo info {
				.thread = thread,
				.tid = nextTid_++,
				.posixHandle = posixHandle,
			};

			async::detach_with_allocator(*kernelAlloc,
				runPosixRequests(info, std::move(posixStream.get<0>())));
			async::detach_with_allocator(*kernelAlloc,
				runObserveLoop(info));

			return _thread.push_back(std::move(info));
		}

		void attachControl(smarter::shared_ptr<Thread, ActiveHandle> thread, LaneHandle lane) {
			auto irq_lock = frg::guard(&irqMutex());
			Universe::Guard universe_guard(thread->getUniverse()->lock);

			controlHandle = thread->getUniverse()->attachDescriptor(universe_guard,
					LaneDescriptor{lane});
		}

		void attachMbus(smarter::shared_ptr<Thread, ActiveHandle> thread) {
			auto irqLock = frg::guard(&irqMutex());
			Universe::Guard universeLock(thread->getUniverse()->lock);

			mbusHandle = thread->getUniverse()->attachDescriptor(universeLock,
					LaneDescriptor{*mbusClient});
		}

		coroutine<int> attachFile(smarter::shared_ptr<Thread, ActiveHandle> thread, OpenFile *file) {
			Handle handle;
			{
				auto irq_lock = frg::guard(&irqMutex());
				Universe::Guard universe_guard(thread->getUniverse()->lock);

				handle = thread->getUniverse()->attachDescriptor(universe_guard,
						LaneDescriptor(file->clientLane));
			}

			for(int fd = 0; fd < (int)openFiles.size(); ++fd) {
				if(openFiles[fd])
					continue;
				openFiles[fd] = file;
				auto copyOutcome = co_await fileTableMemory->copyTo(sizeof(Handle) * fd,
						&handle, sizeof(Handle), WorkQueue::generalQueue()->take());
				assert(copyOutcome);
				co_return fd;
			}

			int fd = openFiles.size();
			openFiles.push(file);
			auto copyOutcome = co_await fileTableMemory->copyTo(sizeof(Handle) * fd,
					&handle, sizeof(Handle), WorkQueue::generalQueue()->take());
			assert(copyOutcome);
			co_return fd;
		}

		frg::string<KernelAlloc> _name;
		frg::vector<ThreadInfo, KernelAlloc> _thread{*kernelAlloc};

		uint64_t nextTid_ = 1;

		Handle mbusHandle;
		Handle controlHandle;
		frg::vector<OpenFile *, KernelAlloc> openFiles;
		smarter::shared_ptr<AllocatedMemory> fileTableMemory;
		VirtualAddr clientFileTable;
	};

	coroutine<void> Process::runPosixRequests(ThreadInfo info, LaneHandle posixLane) {
		while(true) {
			auto [acceptError, conversation] = co_await AcceptSender{posixLane};
			if(acceptError != Error::success) {
				infoLogger() << "thor: Could not accept POSIX lane" << frg::endlog;
				co_return;
			}
			auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
			if(reqError != Error::success) {
				infoLogger() << "thor: Could not receive POSIX request" << frg::endlog;
				co_return;
			}

			auto preamble = bragi::read_preamble(reqBuffer);
			assert(!preamble.error());

			if(preamble.id() == bragi::message_id<managarm::posix::CntRequest>) {
				// This case is only really needed to return an error from SIG_ACTION,
				// since mlibc tries to install a signal handler to support cancellation.

				auto req = bragi::parse_head_only<managarm::posix::CntRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				switch(req->request_type()) {
					case managarm::posix::CntReqType::VM_PROTECT: {
						if(req->mode() & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
							managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
							resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

							frg::string<KernelAlloc> ser(*kernelAlloc);
							resp.SerializeToString(&ser);
							frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
							memcpy(respBuffer.data(), ser.data(), ser.size());
							auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
							// TODO: improve error handling here.
							assert(respError == Error::success);
							break;
						}

						uint32_t native_flags = 0;
						if(req->mode() & PROT_READ)
							native_flags |= VirtualizedPageSpace::kMapProtRead;
						if(req->mode() & PROT_WRITE)
							native_flags |= VirtualizedPageSpace::kMapProtWrite;
						if(req->mode() & PROT_EXEC)
							native_flags |= VirtualizedPageSpace::kMapProtExecute;

						auto space = info.thread->getAddressSpace();
						auto result = co_await space->protect(
							req->address(), req->size(), native_flags);
						// TODO: improve error handling here.
						assert(result);

						managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
						resp.set_error(managarm::posix::Errors::SUCCESS);

						frg::string<KernelAlloc> ser(*kernelAlloc);
						resp.SerializeToString(&ser);
						frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
						memcpy(respBuffer.data(), ser.data(), ser.size());
						auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
						// TODO: improve error handling here.
						assert(respError == Error::success);
						break;
					}
					case managarm::posix::CntReqType::SIG_ACTION:
						infoLogger() << "thor: Unexpected legacy POSIX request "
							<< req->request_type() << frg::endlog;
						[[fallthrough]];
					default: {
						managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
						resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

						frg::string<KernelAlloc> ser(*kernelAlloc);
						resp.SerializeToString(&ser);
						frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
						memcpy(respBuffer.data(), ser.data(), ser.size());
						auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
						// TODO: improve error handling here.
						assert(respError == Error::success);
						break;
					}
				}
			}else if(preamble.id() == bragi::message_id<managarm::posix::OpenAtRequest>) {
				auto [tailError, tailBuffer] = co_await RecvBufferSender{conversation};
				if(tailError != Error::success) {
					infoLogger() << "thor: Could not receive POSIX tail" << frg::endlog;
					co_return;
				}

				auto req = bragi::parse_head_tail<managarm::posix::OpenAtRequest>(
						reqBuffer, tailBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}
				if(req->fd() != -100) {
					infoLogger() << "thor: OpenAt does not support dirfds" << frg::endlog;
					co_return;
				}

				auto module = resolveModule(req->path());
				if(!module) {
					managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
					continue;
				}

				if(module->type == MfsType::directory) {
					auto stream = createStream();
					auto file = frg::construct<initrd::OpenDirectory>(*kernelAlloc,
							static_cast<MfsDirectory *>(module));
					file->clientLane = std::move(stream.get<1>());

					async::detach_with_allocator(*kernelAlloc,
							runDirectoryRequests(file, std::move(stream.get<0>())));

					auto fd = co_await attachFile(info.thread, file);

					managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::SUCCESS);
					resp.set_fd(fd);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
				}else{
					assert(module->type == MfsType::regular);

					auto stream = createStream();
					auto file = frg::construct<initrd::OpenRegular>(*kernelAlloc,
							static_cast<MfsRegular *>(module));
					file->clientLane = std::move(stream.get<1>());

					async::detach_with_allocator(*kernelAlloc,
							runRegularRequests(file, std::move(stream.get<0>())));

					auto fd = co_await attachFile(info.thread, file);

					managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::SUCCESS);
					resp.set_fd(fd);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
				}
			}else if(preamble.id() == bragi::message_id<managarm::posix::FstatAtRequest>) {
				auto [tailError, tailBuffer] = co_await RecvBufferSender{conversation};
				if(tailError != Error::success) {
					infoLogger() << "thor: Could not receive POSIX tail" << frg::endlog;
					co_return;
				}

				auto req = bragi::parse_head_tail<managarm::posix::FstatAtRequest>(
					reqBuffer, tailBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				auto module = resolveModule(req->path());
				if(!module || module->type != MfsType::regular) {
					infoLogger() << "thor: cannot stat file " << req->path() << frg::endlog;
					managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					if(respError != Error::success) {
						infoLogger() << "thor: Could not send POSIX response" << frg::endlog;
						co_return;
					}
					continue;
				}

				auto file = static_cast<MfsRegular *>(module);

				managarm::posix::SvrResponse resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_file_size(file->size());
				resp.set_file_type(managarm::posix::FileType::FT_REGULAR);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				if(respError != Error::success) {
					infoLogger() << "thor: Could not send POSIX response" << frg::endlog;
					co_return;
				}
			}else if(preamble.id() == bragi::message_id<managarm::posix::IsTtyRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::IsTtyRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				assert((size_t)req->fd() < openFiles.size());
				auto file = openFiles[req->fd()];

				managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_mode(file->isTerminal ? 1 : 0);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(preamble.id() == bragi::message_id<managarm::posix::CloseRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::CloseRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				// TODO: for now we just ignore close requests.
				managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(preamble.id() == bragi::message_id<managarm::posix::VmMapRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::VmMapRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				if(!req->size()) {
					managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
					continue;
				}

				uint32_t protFlags = 0;
				if(req->mode() & PROT_READ)
					protFlags |= AddressSpace::kMapProtRead;
				if(req->mode() & PROT_WRITE)
					protFlags |= AddressSpace::kMapProtWrite;
				if(req->mode() & PROT_EXEC)
					protFlags |= AddressSpace::kMapProtExecute;
				if(req->flags() & MAP_FIXED) // MAP_FIXED.
					protFlags |= AddressSpace::kMapFixed;
				else
					protFlags |= AddressSpace::kMapPreferTop;

				if (req->flags() & ~(MAP_ANONYMOUS | MAP_FIXED | MAP_PRIVATE)) {
					managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);

					continue;
				}

				smarter::shared_ptr<MemoryView> fileMemory;
				if(req->flags() & MAP_ANONYMOUS) { // MAP_ANONYMOUS.
					if(req->flags() & MAP_PRIVATE) { // MAP_PRIVATE.
						fileMemory = getZeroMemory();
					}else{
						auto memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc,
								req->size());
						memory->selfPtr = memory;
						fileMemory = std::move(memory);
					}
				}else{
					// TODO: improve error handling here.
					assert((size_t)req->fd() < openFiles.size());
					auto abstractFile = openFiles[req->fd()];
					auto moduleFile = static_cast<initrd::OpenRegular *>(abstractFile);
					fileMemory = moduleFile->module->getMemory();
				}

				smarter::shared_ptr<MemorySlice> slice;
				if(req->flags() & MAP_PRIVATE) { // MAP_PRIVATE.
					auto cowMemory = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc,
							std::move(fileMemory), req->rel_offset(), req->size());
					cowMemory->selfPtr = cowMemory;
					slice = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
							std::move(cowMemory), 0, req->size());
				}else{
					assert(!"TODO: implement shared mappings");
				}

				auto space = info.thread->getAddressSpace();
				auto mapResult = co_await space->map(std::move(slice),
						req->address_hint(), 0, req->size(), protFlags);
				// TODO: improve error handling here.
				assert(mapResult);

				managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_offset(mapResult.value());

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(preamble.id() == bragi::message_id<managarm::posix::GetPidRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::GetPidRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_pid(info.tid);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				if(respError != Error::success) {
					infoLogger() << "thor: Could not send POSIX response" << frg::endlog;
					co_return;
				}
			}else{
				infoLogger() << "thor: Illegal POSIX request type "
						<< preamble.id() << frg::endlog;
				co_return;
			}
		}
	}

	coroutine<void> Process::runObserveLoop(ThreadInfo info) {
		uint64_t currentSeq = 1;
		while(true) {
			auto [error, observedSeq, interrupt] = co_await info.thread->observe(currentSeq);
			assert(error == Error::success);
			currentSeq = observedSeq;

			if(interrupt == kIntrPanic) {
				// Do nothing and stop observing.
				// TODO: Make sure the server is destructed here.
				urgentLogger() << "thor: Panic in server "
						<< name().data() << frg::endlog;
				launchGdbServer(info.thread, _name, WorkQueue::generalQueue()->take());
				break;
			}else if(interrupt == kIntrPageFault) {
				// Do nothing and stop observing.
				// TODO: Make sure the server is destructed here.
				urgentLogger() << "thor: Fault in server "
						<< name().data() << frg::endlog;
				launchGdbServer(info.thread, _name, WorkQueue::generalQueue()->take());
				break;
			}else if(interrupt == kIntrSuperCall + ::posix::superAnonAllocate) { // ANON_ALLOCATE.
				// TODO: Use some always-zero memory for private anonymous mappings.
				auto size = *info.thread->_executor.arg0();
				auto fileMemory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, size);
				fileMemory->selfPtr = fileMemory;
				auto cowMemory = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc,
						std::move(fileMemory), 0, size);
				cowMemory->selfPtr = cowMemory;
				auto slice = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
						std::move(cowMemory), 0, size);

				auto space = info.thread->getAddressSpace();
				auto mapResult = co_await space->map(std::move(slice),
						0, 0, size,
						AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead
						| AddressSpace::kMapProtWrite);
				// TODO: improve error handling here.
				assert(mapResult);

				*info.thread->_executor.result0() = kHelErrNone;
				*info.thread->_executor.result1() = mapResult.value();
				if(auto e = Thread::resumeOther(remove_tag_cast(info.thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + ::posix::superAnonDeallocate) { // ANON_FREE.
				auto address = *info.thread->_executor.arg0();
				auto size = *info.thread->_executor.arg1();
				auto space = info.thread->getAddressSpace();
				auto unmapOutcome = co_await space->unmap(address, size);

				if(!unmapOutcome) {
					assert(unmapOutcome.error() == Error::illegalArgs);
					*info.thread->_executor.result0() = kHelErrIllegalArgs;
				}else{
					*info.thread->_executor.result0() = kHelErrNone;
				}
				*info.thread->_executor.result1() = 0;
				if(auto e = Thread::resumeOther(remove_tag_cast(info.thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + ::posix::superGetProcessData) {
				::posix::ManagarmProcessData data = {
					info.posixHandle,
					mbusHandle,
					nullptr,
					reinterpret_cast<HelHandle *>(clientFileTable),
					nullptr
				};

				auto outcome = co_await info.thread->getAddressSpace()->writeSpace(
						*info.thread->_executor.arg0(), &data, sizeof(::posix::ManagarmProcessData),
						WorkQueue::generalQueue()->take());
				if(!outcome) {
					*info.thread->_executor.result0() = kHelErrFault;
				}else{
					*info.thread->_executor.result0() = kHelErrNone;
				}
				if(auto e = Thread::resumeOther(remove_tag_cast(info.thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + ::posix::superGetServerData) {
				::posix::ManagarmServerData data = {
					controlHandle
				};

				auto outcome = co_await info.thread->getAddressSpace()->writeSpace(
						*info.thread->_executor.arg0(), &data, sizeof(::posix::ManagarmServerData),
						WorkQueue::generalQueue()->take());
				if(!outcome) {
					*info.thread->_executor.result0() = kHelErrFault;
				}else{
					*info.thread->_executor.result0() = kHelErrNone;
				}
				if(auto e = Thread::resumeOther(remove_tag_cast(info.thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + ::posix::superSigMask) { // sigprocmask.
				*info.thread->_executor.result0() = kHelErrNone;
				*info.thread->_executor.result1() = 0;
				if(auto e = Thread::resumeOther(remove_tag_cast(info.thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + ::posix::superGetTid) {
				*info.thread->_executor.result0() = kHelErrNone;
				*info.thread->_executor.result1() = info.tid;
				if(auto e = Thread::resumeOther(remove_tag_cast(info.thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + ::posix::superClone) {
				AbiParameters params;
				params.ip = *info.thread->_executor.arg0();
				params.sp = *info.thread->_executor.arg1();
				params.argument = 0;

				auto new_thread = Thread::create(info.thread->getUniverse().lock(), info.thread->getAddressSpace().lock(), params);
				new_thread->self = remove_tag_cast(new_thread);
				new_thread->flags |= Thread::kFlagServer;
				auto new_info = attachThread(new_thread);

				// see helCreateThread for the reasoning here
				new_thread.ctr()->increment();
				new_thread.ctr()->increment();

				*info.thread->_executor.result0() = kHelErrNone;
				*info.thread->_executor.result1() = new_info.tid;

				LoadBalancer::singleton().connect(new_thread.get(), getCpuData());
				Scheduler::associate(new_thread.get(), &localScheduler.get());

				if(auto e = Thread::resumeOther(remove_tag_cast(new_thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
				if(auto e = Thread::resumeOther(remove_tag_cast(info.thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + ::posix::superExit) {
				break;
			}else if(interrupt == kIntrSuperCall + ::posix::superSigKill) {
				urgentLogger() << "thor: Signal sent by server "
						<< name().data() << frg::endlog;
				launchGdbServer(info.thread, _name, WorkQueue::generalQueue()->take());
				break;
			}else{
				panicLogger() << "thor: Unexpected observation "
						<< (uint32_t)interrupt << frg::endlog;
			}
		}
	}
} // namepace posix

void runService(frg::string<KernelAlloc> name, LaneHandle controlLane,
		smarter::shared_ptr<Thread, ActiveHandle> thread) {
	KernelFiber::run([name, thread, controlLane = std::move(controlLane)] () mutable {
		auto stdioStream = createStream();
		auto stdioFile = frg::construct<StdioFile>(*kernelAlloc);
		stdioFile->clientLane = std::move(stdioStream.get<1>());

		async::detach_with_allocator(*kernelAlloc,
				stdio::runStdioRequests(stdioStream.get<0>()));

		auto process = frg::construct<posix::Process>(*kernelAlloc, std::move(name));
		KernelFiber::asyncBlockCurrent(process->setupAddressSpace(thread));
		process->attachControl(thread, std::move(controlLane));
		process->attachMbus(thread);
		KernelFiber::asyncBlockCurrent(process->attachFile(thread, stdioFile));
		KernelFiber::asyncBlockCurrent(process->attachFile(thread, stdioFile));
		KernelFiber::asyncBlockCurrent(process->attachFile(thread, stdioFile));
		process->attachThread(std::move(thread));

		// Just block this fiber forever (we're still processing worklets).
		FiberBlocker blocker;
		blocker.setup();
		KernelFiber::blockCurrent(&blocker);
	});
}

} // namespace thor
