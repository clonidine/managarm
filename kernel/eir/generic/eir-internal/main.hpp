#pragma once

#include <eir-internal/generic.hpp>
#include <eir/interface.hpp>
#include <frg/string.hpp>
#include <initgraph.hpp>

namespace eir {

extern "C" physaddr_t eirDtbPtr;
extern "C" physaddr_t eirSmbios3Ptr;

struct GlobalInitEngine final : initgraph::Engine {
	void preActivate(initgraph::Node *node) override;
	void onUnreached() override;
};

extern GlobalInitEngine globalInitEngine;

extern "C" [[noreturn]] void eirMain();

extern "C" void eirRunConstructors();

initgraph::Stage *getInitrdAvailableStage();

// achieved by parsing boot protocol-specific data to allow for setting up the CPU and memory
initgraph::Stage *getReservedRegionsKnownStage();

// memory regions and reserved regions have been set up
initgraph::Stage *getMemoryRegionsKnownStage();

// everything needed to construct handoff information for thor is done
initgraph::Stage *getAllocationAvailableStage();

// everything needed to fill out ELF notes and load the kernel image is done
initgraph::Stage *getKernelLoadableStage();

// the handoff information struct can be filled from here on
initgraph::Stage *getInfoStructAvailableStage();

// right before jumping to thor
initgraph::Stage *getEirDoneStage();

extern void *initrd;
extern EirFramebuffer *fb;
extern EirInfo *info_ptr;

extern frg::array<InitialRegion, 32> reservedRegions;
extern size_t nReservedRegions;

extern frg::string_view cmdline;

} // namespace eir
