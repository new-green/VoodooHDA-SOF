// SPDX-License-Identifier: BSD-3-Clause
/*
 * VoodooHDASof — see VoodooHDASof.h.
 *
 * M1: map the DSP BAR (BAR4) and log controller/DSP registers. Strictly read-only:
 * no register or config-space writes happen in this file at step 1.
 */

#include "VoodooHDASof.h"
#include "VoodooHDADevice.h"

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>

#define SOFLOG(fmt, ...)	IOLog("VoodooHDA SOF: " fmt "\n", ##__VA_ARGS__)
#define SOFDBG(fmt, ...)	do { if (mDbg) SOFLOG(fmt, ##__VA_ARGS__); } while (0)

#define INTEL_VID		0x8086U

/* Controllers whose DSP/DMIC path this port has been checked against. */
static const UInt16 kSofDeviceIds[] = {
	0x51C8,		/* Alder Lake PCH-P (HP ProBook 440 G9) */
};

OSDefineMetaClassAndStructors(VoodooHDASof, OSObject)

bool VoodooHDASof::isEnabled(UInt32 deviceId)
{
	UInt32 dummy = 0;

	if (!PE_parse_boot_argn("-voodoosof", &dummy, sizeof dummy))
		return false;
	if ((deviceId & 0xFFFF) != INTEL_VID) {
		SOFLOG("-voodoosof set, but controller %08x is not Intel; SOF disabled", (unsigned)deviceId);
		return false;
	}
	for (size_t i = 0; i < sizeof kSofDeviceIds / sizeof kSofDeviceIds[0]; i++)
		if ((deviceId >> 16) == kSofDeviceIds[i])
			return true;
	SOFLOG("-voodoosof set, but device %04x is not in the supported list; SOF disabled",
	       (unsigned)(deviceId >> 16));
	return false;
}

VoodooHDASof *VoodooHDASof::withDevice(VoodooHDADevice *device)
{
	VoodooHDASof *sof = new VoodooHDASof;

	if (sof && !sof->initWithDevice(device)) {
		sof->release();
		sof = NULL;
	}
	return sof;
}

bool VoodooHDASof::initWithDevice(VoodooHDADevice *device)
{
	UInt32 val = 0;

	if (!OSObject::init() || !device || !device->mPciNub || !device->mRegBase || !device->mBarMap)
		return false;
	mDev = device;
	mPci = device->mPciNub;
	mHdaLen = device->mBarMap->getLength();
	mDbg = PE_parse_boot_argn("-voodoosofdbg", &val, sizeof val);
	mStep = kSofStepMax;
	if (PE_parse_boot_argn("voodoosofstep", &val, sizeof val) && val >= 1 && val <= kSofStepMax)
		mStep = val;
	return true;
}

UInt32 VoodooHDASof::hda32(UInt32 off)
{
	if (off + 4 > mHdaLen)
		return 0xFFFFFFFFU;
	return *(volatile UInt32 *)((UInt8 *)mDev->mRegBase + off);
}

UInt32 VoodooHDASof::dsp32(UInt32 off)
{
	if (!mDsp || off + 4 > mDspLen)
		return 0xFFFFFFFFU;
	return *(volatile UInt32 *)(mDsp + off);
}

bool VoodooHDASof::mapDsp()
{
	UInt32 lo = mPci->configRead32(SOF_DSP_BAR_REG);
	UInt32 hi = mPci->configRead32(SOF_DSP_BAR_REG + 4);

	SOFLOG("BAR4 config: %08x %08x", (unsigned)hi, (unsigned)lo);
	mDspMap = mPci->mapDeviceMemoryWithRegister(SOF_DSP_BAR_REG);	/* uncached by default */
	if (!mDspMap) {
		SOFLOG("error: mapDeviceMemoryWithRegister(BAR4) failed");
		return false;
	}
	mDsp = (volatile UInt8 *)mDspMap->getVirtualAddress();
	mDspLen = mDspMap->getLength();
	SOFLOG("BAR4 mapped: phys 0x%llx len 0x%llx",
	       (unsigned long long)mDspMap->getPhysicalAddress(), (unsigned long long)mDspLen);
	if (!mDsp || mDspLen < SOF_DSP_MBOX_OFFSET + 0x20000) {
		SOFLOG("error: BAR4 too small or unmapped");
		return false;
	}
	return true;
}

/* Walk the HDA extended capability list (snd_hdac_bus_parse_capabilities). */
void VoodooHDASof::findCaps()
{
	UInt32 offset = *(volatile UInt16 *)((UInt8 *)mDev->mRegBase + SOF_HDA_LLCH);

	for (int n = 0; offset && n <= SOF_MAX_CAPS; n++) {
		if ((offset & 3) || offset + 4 > mHdaLen) {
			SOFLOG("cap: bad offset 0x%x, stop", (unsigned)offset);
			break;
		}
		UInt32 cap = hda32(offset);
		if (cap == 0xFFFFFFFFU) {
			SOFLOG("cap: invalid read at 0x%x", (unsigned)offset);
			break;
		}
		UInt32 id = SOF_CAP_HDR_ID(cap);
		const char *name = "?";
		switch (id) {
		case SOF_GTS_CAP_ID:  name = "GTS";  mGtsCap = offset;  break;
		case SOF_ML_CAP_ID:   name = "ML";   mMlCap = offset;   break;
		case SOF_PP_CAP_ID:   name = "PP";   mPpCap = offset;   break;
		case SOF_SPB_CAP_ID:  name = "SPB";  mSpbCap = offset;  break;
		case SOF_DRSM_CAP_ID: name = "DRSM"; mDrsmCap = offset; break;
		}
		SOFLOG("cap @0x%04x: hdr %08x id %u (%s) ver %u next 0x%x", (unsigned)offset, (unsigned)cap,
		       (unsigned)id, name, (unsigned)SOF_CAP_HDR_VER(cap), (unsigned)SOF_CAP_HDR_NXT(cap));
		if (!strcmp(name, "?"))		/* Linux stops on unknown ids as well */
			break;
		offset = SOF_CAP_HDR_NXT(cap);
	}
}

void VoodooHDASof::dumpStreams()
{
	SOFLOG("streams: ISS=%d OSS=%d BSS=%d (SD n regs at 0x80 + n*0x20)",
	       mDev->mInStreamsSup, mDev->mOutStreamsSup, mDev->mBiStreamsSup);
	for (int i = 0; i < mDev->mNumChannels; i++) {
		Channel *ch = &mDev->mChannels[i];
		SOFLOG("  VoodooHDA channel %d: %s SD%d (regs 0x%03x) tag %d", i,
		       ch->direction == PCMDIR_PLAY ? "play" : "rec ", ch->off >> 5, 0x80 + ch->off,
		       ch->streamId);
	}
}

void VoodooHDASof::dumpState(const char *when)
{
	SOFLOG("---- state (%s) ----", when);
	SOFLOG("pci: cmd %04x class %08x PGCTL(0x44) %08x CGCTL(0x48) %08x",
	       mPci->configRead16(kIOPCIConfigCommand), (unsigned)mPci->configRead32(kIOPCIConfigRevisionID),
	       (unsigned)mPci->configRead32(SOF_PCI_PGCTL), (unsigned)mPci->configRead32(SOF_PCI_CGCTL));
	SOFLOG("hda: GCAP %04x GCTL %08x INTCTL %08x INTSTS %08x",
	       (unsigned)(hda32(0x00) & 0xFFFF), (unsigned)hda32(0x08), (unsigned)hda32(0x20),
	       (unsigned)hda32(0x24));
	if (mPpCap)
		SOFLOG("pp:  PPCH %08x PPCTL %08x PPSTS %08x", (unsigned)hda32(mPpCap),
		       (unsigned)hda32(mPpCap + SOF_PP_PPCTL), (unsigned)hda32(mPpCap + SOF_PP_PPSTS));
	if (mMlCap) {
		UInt32 links = (hda32(mMlCap + SOF_ML_MLCD) & 0x1F) + 1;
		SOFLOG("ml:  MLCD %08x (%u links)", (unsigned)hda32(mMlCap + SOF_ML_MLCD), (unsigned)links);
		for (UInt32 l = 0; l < links && l < 8; l++) {
			UInt32 base = mMlCap + SOF_ML_BASE + l * SOF_ML_INTERVAL;
			SOFLOG("  link %u: LCAP %08x LCTL %08x", (unsigned)l, (unsigned)hda32(base + SOF_ML_LCAP),
			       (unsigned)hda32(base + SOF_ML_LCTL));
		}
	}
	if (mDsp) {
		UInt32 cs = dsp32(SOF_DSP_REG_ADSPCS);
		SOFLOG("dsp: ADSPCS %08x (CRST %02x CSTALL %02x SPA %02x CPA %02x)", (unsigned)cs,
		       (unsigned)(cs & 0xFF), (unsigned)((cs >> 8) & 0xFF), (unsigned)((cs >> 16) & 0xFF),
		       (unsigned)((cs >> 24) & 0xFF));
		SOFLOG("dsp: ADSPIC %08x ADSPIS %08x ADSPIC2 %08x ADSPIS2 %08x",
		       (unsigned)dsp32(SOF_DSP_REG_ADSPIC), (unsigned)dsp32(SOF_DSP_REG_ADSPIS),
		       (unsigned)dsp32(SOF_DSP_REG_ADSPIC2), (unsigned)dsp32(SOF_DSP_REG_ADSPIS2));
		SOFLOG("ipc: TDR %08x TDA %08x TDD %08x IDR %08x IDA %08x IDD %08x CTL %08x",
		       (unsigned)dsp32(SOF_CNL_HIPCTDR), (unsigned)dsp32(SOF_CNL_HIPCTDA),
		       (unsigned)dsp32(SOF_CNL_HIPCTDD), (unsigned)dsp32(SOF_CNL_HIPCIDR),
		       (unsigned)dsp32(SOF_CNL_HIPCIDA), (unsigned)dsp32(SOF_CNL_HIPCIDD),
		       (unsigned)dsp32(SOF_CNL_HIPCCTL));
		/* SRAM windows are only meaningful (and only touched) once core 0 is powered. */
		if (cs & SOF_ADSPCS_CPA(1))
			SOFLOG("sram: ROM_STATUS %08x ROM_ERROR %08x", (unsigned)dsp32(SOF_SRAM_ROM_STATUS),
			       (unsigned)dsp32(SOF_SRAM_ROM_ERROR));
		else
			SOFLOG("sram: core 0 not powered, SRAM window not read");
	}
}

bool VoodooHDASof::start()
{
	SOFLOG("enabled (step %u/%u%s), device %08x subsys %08x", (unsigned)mStep, (unsigned)kSofStepMax,
	       mDbg ? ", debug" : "", (unsigned)mDev->mDeviceId, (unsigned)mDev->mSubDeviceId);

	findCaps();
	if (!mPpCap)
		SOFLOG("warning: no PP capability — DSP not exposed by this controller");
	if (!mapDsp())
		return false;
	dumpStreams();
	dumpState("M1 probe");

	if (mStep <= kSofStepProbe) {
		SOFLOG("step %u reached, stopping here", (unsigned)mStep);
		return true;
	}
	return true;
}

void VoodooHDASof::stop()
{
	SOFDBG("stop");
	if (mDspMap) {
		mDspMap->release();
		mDspMap = NULL;
		mDsp = NULL;
	}
}

void VoodooHDASof::free()
{
	stop();
	OSObject::free();
}
