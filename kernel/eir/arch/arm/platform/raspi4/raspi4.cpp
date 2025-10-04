#include <assert.h>
#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>
#include <eir-internal/uart/uart.hpp>
#include <eir/interface.hpp>
#include <frg/eternal.hpp> // for aligned_storage
#include <frg/manual_box.hpp>
#include <frg/tuple.hpp>
#include <render-text.hpp>
#include <stdint.h>

#include <arch/aarch64/mem_space.hpp>
#include <arch/register.hpp>

#include <arch/bit.hpp>
#include <arch/variable.hpp>

// #define RASPI3
#define LOW_PERIPH

namespace eir {

#if defined(RASPI3)
static constexpr inline uintptr_t mmioBase = 0x3f000000;
#elif defined(LOW_PERIPH)
static constexpr inline uintptr_t mmioBase = 0xfe000000;
#else
static constexpr inline uintptr_t mmioBase = 0x47e000000;
#endif

namespace Gpio {
namespace reg {
static constexpr arch::bit_register<uint32_t> sel1{0x04};
static constexpr arch::bit_register<uint32_t> pup_pdn0{0xE4};
} // namespace reg

static constexpr arch::mem_space space{mmioBase + 0x200000};

void configUart0Gpio() {
	arch::field<uint32_t, uint8_t> sel1_p14{12, 3};
	arch::field<uint32_t, uint8_t> sel1_p15{15, 3};

	arch::field<uint32_t, uint8_t> pup_pdn0_p14{28, 2};
	arch::field<uint32_t, uint8_t> pup_pdn0_p15{30, 2};

	// Alt 0
	space.store(reg::sel1, space.load(reg::sel1) / sel1_p14(4) / sel1_p15(4));
	// No pull up/down
	space.store(reg::pup_pdn0, space.load(reg::pup_pdn0) / pup_pdn0_p14(0) / pup_pdn0_p15(0));
}
} // namespace Gpio

namespace Mbox {
static constexpr arch::mem_space space{mmioBase + 0xb880};

namespace reg {
static constexpr arch::bit_register<uint32_t> read{0x00};
static constexpr arch::bit_register<uint32_t> status{0x18};
static constexpr arch::bit_register<uint32_t> write{0x20};
} // namespace reg

enum class Channel { pmi = 0, fb, vuart, vchiq, led, button, touch, property = 8 };

namespace io {
static constexpr arch::field<uint32_t, Channel> channel{0, 4};
static constexpr arch::field<uint32_t, uint32_t> value{4, 28};
} // namespace io

namespace status {
static constexpr arch::field<uint32_t, bool> empty{30, 1};
static constexpr arch::field<uint32_t, bool> full{31, 1};
} // namespace status

void write(Channel channel, uint32_t value) {
	while (space.load(reg::status) & status::full)
		;

	space.store(reg::write, io::channel(channel) | io::value(value >> 4));
}

uint32_t read(Channel channel) {
	while (space.load(reg::status) & status::empty)
		;

	auto f = space.load(reg::read);

	return (f & io::value) << 4;
}
} // namespace Mbox

namespace PropertyMbox {
enum class Clock { uart = 2 };

void setClockFreq(Clock clock, uint32_t freq, bool turbo = false) {
	constexpr uint32_t req_size = 9 * 4;
	frg::aligned_storage<req_size, 16> stor;
	auto ptr = reinterpret_cast<volatile uint32_t *>(stor.buffer);

	*ptr++ = req_size;
	*ptr++ = 0x00000000; // Process request
	*ptr++ = 0x00038002; // Set clock rate
	*ptr++ = 12;
	*ptr++ = 8;
	*ptr++ = static_cast<uint32_t>(clock);
	*ptr++ = freq;
	*ptr++ = turbo;
	*ptr++ = 0x00000000;

	auto val = reinterpret_cast<uint64_t>(stor.buffer);
	assert(!(val & ~(uint64_t(0xFFFFFFF0))));
	Mbox::write(Mbox::Channel::property, val);

	auto ret = Mbox::read(Mbox::Channel::property);
	assert(val == ret);
}

frg::tuple<int, int, void *, size_t>
setupFb(unsigned int width, unsigned int height, unsigned int bpp) {
	constexpr uint32_t req_size = 36 * 4;
	frg::aligned_storage<req_size, 16> stor;
	auto ptr = reinterpret_cast<volatile uint32_t *>(stor.buffer);

	*ptr++ = req_size;
	*ptr++ = 0x00000000; // Process request

	*ptr++ = 0x00048003; // Set physical width/height
	*ptr++ = 8;
	*ptr++ = 0;
	*ptr++ = width;
	*ptr++ = height;

	*ptr++ = 0x00048004; // Set virtual width/height
	*ptr++ = 8;
	*ptr++ = 0;
	*ptr++ = width;
	*ptr++ = height;

	*ptr++ = 0x00048009; // Set virtual offset
	*ptr++ = 8;
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;

	*ptr++ = 0x00048005; // Set depth
	*ptr++ = 4;
	*ptr++ = 0;
	*ptr++ = bpp;

	*ptr++ = 0x00048006; // Set pixel order
	*ptr++ = 4;
	*ptr++ = 0;
	*ptr++ = 0; // RGB

	*ptr++ = 0x00040001; // Allocate buffer
	*ptr++ = 8;
	*ptr++ = 0;
	*ptr++ = 0x1000;
	*ptr++ = 0;

	*ptr++ = 0x00040008; // Get pitch
	*ptr++ = 4;
	*ptr++ = 0;
	*ptr++ = 0;

	*ptr++ = 0;

	*ptr++ = 0x00000000;

	auto val = reinterpret_cast<uint64_t>(stor.buffer);
	assert(!(val & ~(uint64_t(0xFFFFFFF0))));
	Mbox::write(Mbox::Channel::property, val);

	auto ret = Mbox::read(Mbox::Channel::property);
	assert(val == ret);

	ptr = reinterpret_cast<volatile uint32_t *>(ret);

	auto fbPtr = 0;

	// if depth is not the expected depth, pretend we failed
	if (ptr[20] == bpp) { // depth == expected depth
#ifndef RASPI3
		// Translate legacy master view address into our address space
		fbPtr = ptr[28] - 0xC0000000;
#else
		fbPtr = ptr[28];
#endif
	}

	return frg::make_tuple(
	    int(ptr[5]), int(ptr[6]), reinterpret_cast<void *>(fbPtr), size_t(ptr[33])
	);
}

template <size_t MaxSize>
size_t getCmdline(void *dest)
    requires(!(MaxSize & 3))
{
	constexpr uint32_t req_size = 5 * 4 + MaxSize;
	frg::aligned_storage<req_size, 16> stor;
	memset(stor.buffer, 0, req_size);

	auto ptr = reinterpret_cast<volatile uint32_t *>(stor.buffer);

	*ptr++ = req_size;
	*ptr++ = 0x00000000; // Process request

	*ptr++ = 0x00050001; // Get commandline
	*ptr++ = MaxSize;

	auto val = reinterpret_cast<uint64_t>(stor.buffer);
	assert(!(val & ~(uint64_t(0xFFFFFFF0))));
	Mbox::write(Mbox::Channel::property, val);

	auto ret = Mbox::read(Mbox::Channel::property);
	assert(val == ret);

	ptr = reinterpret_cast<volatile uint32_t *>(ret);

	auto data = reinterpret_cast<char *>(ret + 20);
	auto totalLen = ptr[3];
	auto cmdlineLen = strlen(data);

	assert(totalLen <= MaxSize);
	memcpy(dest, data, cmdlineLen + 1);

	return cmdlineLen;
}
} // namespace PropertyMbox

namespace {

static initgraph::Task setupFramebuffer{
    &globalInitEngine,
    "raspi4.setup-framebuffer",
    initgraph::Requires{getCmdlineAvailableStage()},
    initgraph::Entails{getEirDoneStage()},
    [] {
	    DeviceTree dt{physToVirt<void>(eirDtbPtr)};
	    auto rootNode = dt.rootNode();

	    bool isRaspi4 = false;
	    if (auto compatibleProperty = rootNode.findProperty("compatible"); compatibleProperty) {
		    size_t i = 0;
		    while (true) {
			    auto compatibleStr = compatibleProperty->asString(i);
			    if (!compatibleStr)
				    break;
			    ++i;
			    if (*compatibleStr == "raspberrypi,4-model-b")
				    isRaspi4 = true;
		    }
	    }

	    if (!isRaspi4)
		    return;

	    infoLogger() << "Attempting to set up a framebuffer:" << frg::endlog;
	    unsigned int fb_width = 0, fb_height = 0;

	    // Parse the command line.
	    {
		    const char *l = cmdline.data();
		    while (true) {
			    while (*l && *l == ' ')
				    l++;
			    if (!(*l))
				    break;

			    const char *s = l;
			    while (*s && *s != ' ')
				    s++;

			    frg::string_view token{l, static_cast<size_t>(s - l)};

			    if (auto equals = token.find_first('='); equals != size_t(-1)) {
				    auto key = token.sub_string(0, equals);
				    auto value = token.sub_string(equals + 1, token.size() - equals - 1);

				    if (key == "bcm2708_fb.fbwidth") {
					    if (auto width = value.to_number<unsigned int>(); width)
						    fb_width = *width;
				    } else if (key == "bcm2708_fb.fbheight") {
					    if (auto height = value.to_number<unsigned int>(); height)
						    fb_height = *height;
				    }
			    }

			    l = s;
		    }
	    }

	    if (!fb_width || !fb_height) {
		    infoLogger() << "No display attached" << frg::endlog;
		    return;
	    }

	    auto [actualW, actualH, ptr, pitch] = PropertyMbox::setupFb(fb_width, fb_height, 32);
	    if (!ptr || !pitch) {
		    infoLogger() << "Mode setting failed..." << frg::endlog;
		    return;
	    }
	    infoLogger() << "Success!" << frg::endlog;

	    EirFramebuffer fb;
	    fb.fbAddress = reinterpret_cast<uintptr_t>(ptr);
	    fb.fbWidth = actualW;
	    fb.fbHeight = actualH;
	    fb.fbPitch = pitch;
	    fb.fbBpp = 32;
	    fb.fbType = 0;
	    initFramebuffer(fb);

	    infoLogger() << "Framebuffer pointer: " << ptr << frg::endlog;
	    infoLogger() << "Framebuffer pitch: " << pitch << frg::endlog;
	    infoLogger() << "Framebuffer width: " << actualW << frg::endlog;
	    infoLogger() << "Framebuffer height: " << actualH << frg::endlog;
    }
};

} // namespace

} // namespace eir
