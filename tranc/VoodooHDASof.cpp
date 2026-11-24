// SPDX-License-Identifier: BSD-3-Clause
/*
 * VoodooHDASof — see VoodooHDASof.h.
 *
 * M1 (step 1): map BAR4, read-only register dump.
 * M2 (steps 2-3): DSP cold boot, port of hda_dsp_cl_boot_firmware_iccmax():
 *   pre_fw_run (clock/power gating off) -> ICCMAX capture stream -> code loader stream
 *   -> cl_dsp_init (core 0 power up, ROM IPC purge, run, ROM INIT_DONE)
 *   -> code loader DMA -> FW_ENTERED -> cleanup -> FW_READY (IPC3) -> post_fw_run.
 *
 * Deliberate differences from Linux, to stay out of VoodooHDA's interrupt path:
 *   - no stream interrupt enables (SD_CTL int bits, INTCTL), no PPCTL.PIE, no HIPCCTL/ADSPIC:
 *     everything is polled.
 *   - SOF uses the highest-numbered input/output stream descriptors, which VoodooHDA
 *     does not use (VoodooHDA allocates from SD0 / SD(ISS) upwards).
 */

#include "VoodooHDASof.h"
#include "VoodooHDADevice.h"

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>

#define SOFLOG(fmt, ...)	IOLog("VoodooHDA SOF: " fmt "\n", ##__VA_ARGS__)
#define SOFDBG(fmt, ...)	do { if (mDbg) SOFLOG(fmt, ##__VA_ARGS__); } while (0)

#define INTEL_VID		0x8086U

extern "C" {
extern const unsigned char gSofFirmware[];
extern const unsigned int gSofFirmwareSize;
}

/* Controllers whose DSP/DMIC path this port has been checked against. */
static const UInt16 kSofDeviceIds[] = {
	0x51C8,		/* Alder Lake PCH-P (HP ProBook 440 G9) */
};

/* struct sof_bdl / sof_intel_dsp_bdl */
struct SofBdlEntry {
	UInt32 addrLo;
	UInt32 addrHi;
	UInt32 size;
	UInt32 ioc;
} __attribute__((packed));

/* include/sound/sof/info.h */
struct SofIpcFwVersion {
	UInt32 hdrSize;
	UInt16 major, minor, micro, build;
	UInt8 date[12];
	UInt8 time[10];
	UInt8 tag[6];
	UInt32 abiVersion;
	UInt32 srcHash;
	UInt32 reserved[3];
} __attribute__((packed));

struct SofIpcFwReady {
	UInt32 hdrSize;
	UInt32 hdrCmd;
	UInt32 dspboxOffset;
	UInt32 hostboxOffset;
	UInt32 dspboxSize;
	UInt32 hostboxSize;
	SofIpcFwVersion version;
	UInt64 flags;
	UInt32 reserved[4];
} __attribute__((packed));

OSDefineMetaClassAndStructors(VoodooHDASof, OSObject)

/* ============================================================================ */
/* setup                                                                        */
/* ============================================================================ */

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
	mLoader.index = mIccmax.index = -1;
	return true;
}

/* ============================================================================ */
/* register access                                                              */
/* ============================================================================ */

UInt32 VoodooHDASof::hda32(UInt32 off)
{
	if (off + 4 > mHdaLen)
		return 0xFFFFFFFFU;
	return *(volatile UInt32 *)((UInt8 *)mDev->mRegBase + off);
}

UInt16 VoodooHDASof::hda16(UInt32 off)
{
	if (off + 2 > mHdaLen)
		return 0xFFFF;
	return *(volatile UInt16 *)((UInt8 *)mDev->mRegBase + off);
}

UInt8 VoodooHDASof::hda8(UInt32 off)
{
	if (off + 1 > mHdaLen)
		return 0xFF;
	return *(volatile UInt8 *)((UInt8 *)mDev->mRegBase + off);
}

void VoodooHDASof::hdaW32(UInt32 off, UInt32 val)
{
	if (off + 4 <= mHdaLen)
		*(volatile UInt32 *)((UInt8 *)mDev->mRegBase + off) = val;
}

void VoodooHDASof::hdaW16(UInt32 off, UInt16 val)
{
	if (off + 2 <= mHdaLen)
		*(volatile UInt16 *)((UInt8 *)mDev->mRegBase + off) = val;
}

void VoodooHDASof::hdaW8(UInt32 off, UInt8 val)
{
	if (off + 1 <= mHdaLen)
		*(volatile UInt8 *)((UInt8 *)mDev->mRegBase + off) = val;
}

void VoodooHDASof::hdaUpdate32(UInt32 off, UInt32 mask, UInt32 val)
{
	hdaW32(off, (hda32(off) & ~mask) | (val & mask));
}

UInt32 VoodooHDASof::dsp32(UInt32 off)
{
	if (!mDsp || off + 4 > mDspLen)
		return 0xFFFFFFFFU;
	return *(volatile UInt32 *)(mDsp + off);
}

void VoodooHDASof::dspW32(UInt32 off, UInt32 val)
{
	if (mDsp && off + 4 <= mDspLen)
		*(volatile UInt32 *)(mDsp + off) = val;
}

void VoodooHDASof::dspUpdate32(UInt32 off, UInt32 mask, UInt32 val)
{
	dspW32(off, (dsp32(off) & ~mask) | (val & mask));
}

void VoodooHDASof::pciUpdate32(UInt32 off, UInt32 mask, UInt32 val)
{
	UInt32 old = mPci->configRead32((UInt8)off);
	UInt32 nv = (old & ~mask) | (val & mask);

	if (nv != old)
		mPci->configWrite32((UInt8)off, nv);
	SOFDBG("pci[0x%02x] %08x -> %08x", (unsigned)off, (unsigned)old, (unsigned)nv);
}

/* Poll until (reg & mask) == want. Short timeouts spin, long ones sleep in 1 ms steps. */
bool VoodooHDASof::pollHda(UInt32 off, UInt32 mask, UInt32 want, UInt32 timeoutUs, UInt32 *last)
{
	UInt32 v = 0, waited = 0;
	bool spin = timeoutUs <= 2000;

	for (;;) {
		v = hda32(off);
		if ((v & mask) == want)
			break;
		if (waited >= timeoutUs)
			break;
		if (spin) { IODelay(10); waited += 10; } else { IOSleep(1); waited += 1000; }
	}
	if (last)
		*last = v;
	return (v & mask) == want;
}

bool VoodooHDASof::pollDsp(UInt32 off, UInt32 mask, UInt32 want, UInt32 timeoutUs, UInt32 *last)
{
	UInt32 v = 0, waited = 0;
	bool spin = timeoutUs <= 2000;

	for (;;) {
		v = dsp32(off);
		if ((v & mask) == want)
			break;
		if (waited >= timeoutUs)
			break;
		if (spin) { IODelay(10); waited += 10; } else { IOSleep(1); waited += 1000; }
	}
	if (last)
		*last = v;
	return (v & mask) == want;
}

/* ============================================================================ */
/* M1: probe                                                                    */
/* ============================================================================ */

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
	UInt32 offset = hda16(SOF_HDA_LLCH);

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
		const char *name = NULL;
		switch (id) {
		case SOF_GTS_CAP_ID:  name = "GTS";  mGtsCap = offset;  break;
		case SOF_ML_CAP_ID:   name = "ML";   mMlCap = offset;   break;
		case SOF_PP_CAP_ID:   name = "PP";   mPpCap = offset;   break;
		case SOF_SPB_CAP_ID:  name = "SPB";  mSpbCap = offset;  break;
		case SOF_DRSM_CAP_ID: name = "DRSM"; mDrsmCap = offset; break;
		}
		SOFLOG("cap @0x%04x: hdr %08x id %u (%s) ver %u next 0x%x", (unsigned)offset, (unsigned)cap,
		       (unsigned)id, name ? name : "?", (unsigned)SOF_CAP_HDR_VER(cap),
		       (unsigned)SOF_CAP_HDR_NXT(cap));
		if (!name)		/* Linux stops on unknown ids as well */
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
	SOFLOG("hda: GCAP %04x GCTL %08x INTCTL %08x INTSTS %08x EM2 %08x LTRP %02x",
	       (unsigned)hda16(0x00), (unsigned)hda32(0x08), (unsigned)hda32(0x20), (unsigned)hda32(0x24),
	       (unsigned)hda32(SOF_VS_EM2), (unsigned)hda8(SOF_VS_LTRP));
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
		if (cs & SOF_ADSPCS_CPA(SOF_CORE0))
			SOFLOG("sram: ROM_STATUS %08x ROM_ERROR %08x", (unsigned)dsp32(SOF_SRAM_ROM_STATUS),
			       (unsigned)dsp32(SOF_SRAM_ROM_ERROR));
		else
			SOFLOG("sram: core 0 not powered, SRAM window not read");
	}
}

/* ============================================================================ */
/* controller / DSP core                                                        */
/* ============================================================================ */

/* hda_dsp_ctrl_clock_power_gating() — pre_fw_run(false) / post_fw_run(true) */
void VoodooHDASof::clockPowerGating(bool enable)
{
	pciUpdate32(SOF_PCI_CGCTL, SOF_PCI_CGCTL_ADSPDCGE, enable ? SOF_PCI_CGCTL_ADSPDCGE : 0);
	if (!enable) {
		mSavedEm2L1 = (hda32(SOF_VS_EM2) & SOF_VS_EM2_L1SEN) != 0;
		hdaUpdate32(SOF_VS_EM2, SOF_VS_EM2_L1SEN, 0);
	} else if (mSavedEm2L1) {
		hdaUpdate32(SOF_VS_EM2, SOF_VS_EM2_L1SEN, SOF_VS_EM2_L1SEN);
	}
	pciUpdate32(SOF_PCI_PGCTL, SOF_PCI_PGCTL_ADSPPGD, enable ? 0 : SOF_PCI_PGCTL_ADSPPGD);
	mGatingDisabled = !enable;
}

bool VoodooHDASof::coreIsEnabled(UInt32 cm)
{
	UInt32 v = dsp32(SOF_DSP_REG_ADSPCS);

	return (v & SOF_ADSPCS_SPA(cm)) == SOF_ADSPCS_SPA(cm) &&
	       (v & SOF_ADSPCS_CPA(cm)) == SOF_ADSPCS_CPA(cm) &&
	       !(v & SOF_ADSPCS_CRST(cm)) && !(v & SOF_ADSPCS_CSTALL(cm));
}

/* hda_dsp_core_power_up() */
bool VoodooHDASof::corePowerUp(UInt32 cm)
{
	UInt32 v;

	dspUpdate32(SOF_DSP_REG_ADSPCS, SOF_ADSPCS_SPA(cm), SOF_ADSPCS_SPA(cm));
	if (!pollDsp(SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CPA(cm), SOF_ADSPCS_CPA(cm), SOF_RESET_TIMEOUT_US, &v)) {
		SOFLOG("error: core power up timeout, ADSPCS %08x", (unsigned)v);
		return false;
	}
	mDspPowered = true;
	SOFDBG("core power up: ADSPCS %08x", (unsigned)v);
	return true;
}

/* hda_dsp_core_run(): reset leave + unstall */
bool VoodooHDASof::coreRun(UInt32 cm)
{
	UInt32 v;

	dspUpdate32(SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CRST(cm), 0);
	if (!pollDsp(SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CRST(cm), 0, SOF_RESET_TIMEOUT_US, &v)) {
		SOFLOG("error: core reset leave timeout, ADSPCS %08x", (unsigned)v);
		return false;
	}
	dspUpdate32(SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CSTALL(cm), 0);
	if (!coreIsEnabled(cm)) {
		SOFLOG("error: core not enabled after run, ADSPCS %08x", (unsigned)dsp32(SOF_DSP_REG_ADSPCS));
		coreStallReset(cm);
		return false;
	}
	SOFDBG("core run: ADSPCS %08x", (unsigned)dsp32(SOF_DSP_REG_ADSPCS));
	return true;
}

/* hda_dsp_core_stall_reset() */
bool VoodooHDASof::coreStallReset(UInt32 cm)
{
	UInt32 v;

	dspUpdate32(SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CSTALL(cm), SOF_ADSPCS_CSTALL(cm));
	dspUpdate32(SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CRST(cm), SOF_ADSPCS_CRST(cm));
	if (!pollDsp(SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CRST(cm), SOF_ADSPCS_CRST(cm), SOF_RESET_TIMEOUT_US, &v)) {
		SOFLOG("error: core reset enter timeout, ADSPCS %08x", (unsigned)v);
		return false;
	}
	return true;
}

/* hda_dsp_core_reset_power_down() */
bool VoodooHDASof::coreResetPowerDown(UInt32 cm)
{
	UInt32 v;
	bool ok = coreStallReset(cm);

	dspUpdate32(SOF_DSP_REG_ADSPCS, SOF_ADSPCS_SPA(cm), 0);
	if (!pollDsp(SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CPA(cm), 0, SOF_PD_TIMEOUT_US, &v)) {
		SOFLOG("error: core power down timeout, ADSPCS %08x", (unsigned)v);
		ok = false;
	} else {
		mDspPowered = false;
	}
	SOFDBG("core reset+power down: ADSPCS %08x", (unsigned)dsp32(SOF_DSP_REG_ADSPCS));
	return ok;
}

/* hda_ssp_set_cbp_cfp(): all SSPs to clock consumer mode once the DSP is powered */
void VoodooHDASof::sspSetConsumer()
{
	for (UInt32 i = 0; i < SOF_SSP_COUNT; i++)
		dspUpdate32(SOF_SSP_BASE + i * SOF_SSP_SIZE + SOF_SSP_SSC1, SOF_SSP_CBP_CFP, SOF_SSP_CBP_CFP);
}

/* ============================================================================ */
/* streams                                                                      */
/* ============================================================================ */

bool VoodooHDASof::pickStreams()
{
	int iss = mDev->mInStreamsSup, oss = mDev->mOutStreamsSup;

	if (iss < 2 || oss < 2) {
		SOFLOG("error: not enough streams (ISS %d OSS %d)", iss, oss);
		return false;
	}
	/* highest capture and highest playback descriptor */
	mIccmax.index = iss - 1;
	mIccmax.tag = iss;
	mIccmax.playback = false;
	mLoader.index = iss + oss - 1;
	mLoader.tag = oss;
	mLoader.playback = true;

	for (int i = 0; i < mDev->mNumChannels; i++) {
		int sd = mDev->mChannels[i].off >> 5;
		if (sd == mIccmax.index || sd == mLoader.index) {
			SOFLOG("error: SD%d is used by VoodooHDA channel %d", sd, i);
			mIccmax.index = mLoader.index = -1;
			return false;
		}
	}
	mIccmax.sd = SOF_SD_OFFSET(mIccmax.index);
	mLoader.sd = SOF_SD_OFFSET(mLoader.index);
	SOFLOG("streams: code loader SD%d tag %d, ICCMAX SD%d tag %d", mLoader.index, mLoader.tag,
	       mIccmax.index, mIccmax.tag);
	return true;
}

bool VoodooHDASof::streamAlloc(SofStream *s, UInt32 size, const char *what)
{
	s->size = size;
	s->frags = 0;
	s->buf = mDev->allocateDmaMemory(size, what, kIOMapInhibitCache);
	s->bdl = mDev->allocateDmaMemory(SOF_BDL_SIZE, "SOF BDL", kIOMapInhibitCache);
	if (!s->buf || !s->bdl) {
		SOFLOG("error: DMA alloc failed for %s (%u bytes)", what, (unsigned)size);
		streamFree(s);
		return false;
	}
	SOFDBG("%s: buf phys 0x%llx size 0x%x, bdl phys 0x%llx", what,
	       (unsigned long long)s->buf->physAddr, (unsigned)size, (unsigned long long)s->bdl->physAddr);
	return true;
}

void VoodooHDASof::streamFree(SofStream *s)
{
	if (s->buf) {
		mDev->freeDmaMemory(s->buf);
		s->buf = NULL;
	}
	if (s->bdl) {
		mDev->freeDmaMemory(s->bdl);
		s->bdl = NULL;
	}
}

/*
 * hda_dsp_stream_setup_bdl(): the buffer is physically contiguous; split it into
 * two equal periods like the reference trace ("periods: 2"). No IOC.
 */
int VoodooHDASof::streamSetupBdl(SofStream *s)
{
	SofBdlEntry *bdl = (SofBdlEntry *)s->bdl->virtAddr;
	UInt32 half = s->size / 2;

	for (int i = 0; i < 2; i++) {
		UInt64 addr = s->buf->physAddr + (UInt64)i * half;
		bdl[i].addrLo = (UInt32)addr;
		bdl[i].addrHi = (UInt32)(addr >> 32);
		bdl[i].size = (i == 1) ? s->size - half : half;
		bdl[i].ioc = 0;
	}
	s->frags = 2;
	return s->frags;
}

/* hda_dsp_stream_reset() */
bool VoodooHDASof::streamReset(SofStream *s)
{
	int timeout;

	hdaUpdate32(s->sd, SOF_SD_CTL_SRST, SOF_SD_CTL_SRST);
	for (timeout = 300; timeout && !(hda32(s->sd) & SOF_SD_CTL_SRST); timeout--)
		IODelay(1);
	if (!timeout) {
		SOFLOG("error: SD%d reset enter timeout", s->index);
		return false;
	}
	hdaUpdate32(s->sd, SOF_SD_CTL_SRST, 0);
	IODelay(3);
	for (timeout = 300; timeout && (hda32(s->sd) & SOF_SD_CTL_SRST); timeout--)
		IODelay(1);
	if (!timeout) {
		SOFLOG("error: SD%d reset leave timeout", s->index);
		return false;
	}
	return true;
}

/* hda_cl_prepare() + hda_dsp_stream_hw_params() + hda_dsp_stream_spib_config(ENABLE) */
bool VoodooHDASof::loaderPrepare(const UInt8 *fw, UInt32 fwSize)
{
	SofStream *s = &mLoader;
	UInt32 mask = 1U << s->index, v;

	if (!streamAlloc(s, fwSize, "SOF code loader"))
		return false;
	memcpy((void *)s->buf->virtAddr, fw, fwSize);

	hdaUpdate32(mPpCap + SOF_PP_PPCTL, mask, mask);		/* decouple */
	hdaUpdate32(s->sd, SOF_SD_INT_MASK | SOF_SD_CTL_DMA_START, 0);
	if (!pollHda(s->sd, SOF_SD_CTL_DMA_START, 0, SOF_STREAM_RUN_TIMEOUT_US, &v)) {
		SOFLOG("error: SD%d DMA stop timeout (ctl %08x)", s->index, (unsigned)v);
		return false;
	}
	hdaW8(s->sd + SOF_SD_STS, SOF_SD_INT_MASK);
	if (!streamReset(s))
		return false;
	hdaW32(s->sd + SOF_SD_BDLPL, 0);
	hdaW32(s->sd + SOF_SD_BDLPU, 0);
	hdaUpdate32(s->sd, SOF_SD_INT_MASK | SOF_SD_CTL_DMA_START, 0);
	if (!pollHda(s->sd, SOF_SD_CTL_DMA_START, 0, SOF_STREAM_RUN_TIMEOUT_US, &v)) {
		SOFLOG("error: SD%d DMA stop timeout 2 (ctl %08x)", s->index, (unsigned)v);
		return false;
	}
	hdaW8(s->sd + SOF_SD_STS, SOF_SD_INT_MASK);

	streamSetupBdl(s);
	hdaUpdate32(s->sd, SOF_SD_CTL_TAG_MASK, (UInt32)s->tag << SOF_SD_CTL_TAG_SHIFT);
	hdaW32(s->sd + SOF_SD_CBL, s->size);
	hdaW16(s->sd + SOF_SD_FORMAT, SOF_CL_STREAM_FORMAT);
	hdaW16(s->sd + SOF_SD_LVI, (UInt16)(s->frags - 1));
	hdaW32(s->sd + SOF_SD_BDLPL, (UInt32)s->bdl->physAddr);
	hdaW32(s->sd + SOF_SD_BDLPU, (UInt32)(s->bdl->physAddr >> 32));
	/* no SD interrupt enables (see header comment) */

	if (mSpbCap) {
		hdaUpdate32(mSpbCap + SOF_SPB_SPBFCCTL, mask, mask);
		hdaW32(mSpbCap + SOF_SPB_SPIB(s->index), s->size);
	}
	SOFLOG("code loader ready: SD%d ctl %08x cbl %08x lvi %04x bdl %08x:%08x fifos %04x",
	       s->index, (unsigned)hda32(s->sd), (unsigned)hda32(s->sd + SOF_SD_CBL),
	       (unsigned)hda16(s->sd + SOF_SD_LVI), (unsigned)hda32(s->sd + SOF_SD_BDLPU),
	       (unsigned)hda32(s->sd + SOF_SD_BDLPL), (unsigned)hda16(s->sd + SOF_SD_FIFOSIZE));
	return true;
}

/* hda_cl_trigger(): start = DMA run only (no interrupts); stop = hda_dsp_stream_trigger(STOP) */
bool VoodooHDASof::loaderTrigger(bool start)
{
	SofStream *s = &mLoader;
	UInt32 v;

	if (start) {
		hdaUpdate32(s->sd, SOF_SD_CTL_DMA_START, SOF_SD_CTL_DMA_START);
		return true;
	}
	hdaUpdate32(s->sd, SOF_SD_CTL_DMA_START | SOF_SD_INT_MASK, 0);
	if (!pollHda(s->sd, SOF_SD_CTL_DMA_START, 0, SOF_STREAM_RUN_TIMEOUT_US, &v)) {
		SOFLOG("error: code loader DMA stop timeout (ctl %08x)", (unsigned)v);
		return false;
	}
	hdaW8(s->sd + SOF_SD_STS, SOF_SD_INT_MASK);
	return true;
}

/* hda_cl_cleanup() for the playback loader stream */
void VoodooHDASof::loaderCleanup()
{
	SofStream *s = &mLoader;

	if (s->index < 0 || !s->buf)
		return;
	if (hda32(s->sd) & SOF_SD_CTL_DMA_START)
		loaderTrigger(false);
	if (mSpbCap) {
		hdaUpdate32(mSpbCap + SOF_SPB_SPBFCCTL, 1U << s->index, 0);
		hdaW32(mSpbCap + SOF_SPB_SPIB(s->index), 0);
	}
	hdaW32(s->sd + SOF_SD_BDLPL, 0);
	hdaW32(s->sd + SOF_SD_BDLPU, 0);
	hdaW32(s->sd, 0);
	hdaUpdate32(mPpCap + SOF_PP_PPCTL, 1U << s->index, 0);	/* re-couple: leave SD as found */
	streamFree(s);
}

/* hda_dsp_iccmax_stream_hw_params(): 4 KB capture stream running during FW boot */
bool VoodooHDASof::iccmaxStart()
{
	SofStream *s = &mIccmax;
	UInt32 mask = 1U << s->index;

	if (!streamAlloc(s, 4096, "SOF ICCMAX"))
		return false;
	hdaW32(s->sd + SOF_SD_BDLPL, 0);
	hdaW32(s->sd + SOF_SD_BDLPU, 0);
	streamSetupBdl(s);
	hdaW32(s->sd + SOF_SD_BDLPL, (UInt32)s->bdl->physAddr);
	hdaW32(s->sd + SOF_SD_BDLPU, (UInt32)(s->bdl->physAddr >> 32));
	hdaW32(s->sd + SOF_SD_CBL, s->size);
	hdaW16(s->sd + SOF_SD_LVI, (UInt16)(s->frags - 1));
	hdaUpdate32(mPpCap + SOF_PP_PPCTL, mask, mask);
	/* snd_sof_dsp_update8(LTRP, GB_MASK, 95): the value is masked, as in Linux (95 & 0x3F) */
	hdaW8(SOF_VS_LTRP, (UInt8)((hda8(SOF_VS_LTRP) & ~SOF_VS_LTRP_GB_MASK) |
				   (SOF_LTRP_GB_VALUE_US & SOF_VS_LTRP_GB_MASK)));
	hdaUpdate32(s->sd, SOF_SD_CTL_DMA_START, SOF_SD_CTL_DMA_START);
	SOFDBG("ICCMAX started: SD%d ctl %08x", s->index, (unsigned)hda32(s->sd));
	return true;
}

/* hda_cl_cleanup() for the capture ICCMAX stream + LTRP guardband restore */
void VoodooHDASof::iccmaxCleanup()
{
	SofStream *s = &mIccmax;
	UInt32 v;

	if (s->index < 0 || !s->buf)
		return;
	hdaUpdate32(s->sd, SOF_SD_CTL_DMA_START, 0);
	pollHda(s->sd, SOF_SD_CTL_DMA_START, 0, SOF_STREAM_RUN_TIMEOUT_US, &v);
	hdaW32(s->sd + SOF_SD_BDLPL, 0);
	hdaW32(s->sd + SOF_SD_BDLPU, 0);
	hdaW32(s->sd, 0);
	hdaUpdate32(mPpCap + SOF_PP_PPCTL, 1U << s->index, 0);
	hdaW8(SOF_VS_LTRP, mSavedLtrp);	/* restore the whole original byte */
	streamFree(s);
}

/* ============================================================================ */
/* boot                                                                         */
/* ============================================================================ */

/* cl_dsp_init() (non-IMR) */
bool VoodooHDASof::clDspInit(int tag)
{
	UInt32 v;

	if (!corePowerUp(SOF_CORE0))
		goto err;
	sspSetConsumer();

	/* ROM_CONTROL: purge FW, DMA id = tag - 1 */
	dspW32(SOF_CNL_HIPCIDR, SOF_IPC_BUSY | SOF_ROM_IPC_CONTROL | SOF_ROM_IPC_PURGE_FW |
				((UInt32)(tag - 1) << 9));

	if (!coreRun(SOF_CORE0))
		goto err;

	if (!pollDsp(SOF_CNL_HIPCIDA, SOF_IPC_DONE, SOF_IPC_DONE, SOF_INIT_TIMEOUT_US, &v)) {
		SOFLOG("error: timeout for ROM IPC done (HIPCIDA %08x)", (unsigned)v);
		goto err;
	}
	dspUpdate32(SOF_CNL_HIPCIDA, SOF_IPC_DONE, SOF_IPC_DONE);	/* forced W1 */
	/* (step 6, IPC interrupt enable, intentionally skipped: polled) */

	if (!pollDsp(SOF_SRAM_ROM_STATUS, 0xFFFFFFU, SOF_FSR_INIT_DONE, SOF_ROM_INIT_TIMEOUT_US, &v)) {
		SOFLOG("error: ROM init timeout, ROM_STATUS %08x ROM_ERROR %08x", (unsigned)v,
		       (unsigned)dsp32(SOF_SRAM_ROM_ERROR));
		goto err;
	}
	SOFLOG("ROM init done: ROM_STATUS %08x", (unsigned)v);
	return true;

err:
	dumpState("cl_dsp_init failed");
	coreResetPowerDown(SOF_CORE0);
	return false;
}

void VoodooHDASof::abortBoot(const char *why)
{
	SOFLOG("boot aborted: %s", why);
	if (mDspPowered)
		coreResetPowerDown(SOF_CORE0);
	loaderCleanup();
	iccmaxCleanup();
	if (mGatingDisabled)
		clockPowerGating(true);
}

/* Poll for the IPC3 FW_READY doorbell, read sof_ipc_fw_ready, ack (cnl_ipc_host_done). */
bool VoodooHDASof::waitFwReady()
{
	UInt32 tdr, tdd, words[sizeof(SofIpcFwReady) / 4];
	SofIpcFwReady *r = (SofIpcFwReady *)words;
	char tag[7];

	if (!pollDsp(SOF_CNL_HIPCTDR, SOF_IPC_BUSY, SOF_IPC_BUSY, SOF_BOOT_TIMEOUT_US, &tdr)) {
		SOFLOG("error: no FW_READY (HIPCTDR %08x, ROM_STATUS %08x)", (unsigned)tdr,
		       (unsigned)dsp32(SOF_SRAM_ROM_STATUS));
		return false;
	}
	tdd = dsp32(SOF_CNL_HIPCTDD);
	SOFLOG("ipc rx: %08x ext %08x", (unsigned)(tdr & SOF_IPC_MSG_MASK), (unsigned)(tdd & SOF_IPC_MSG_MASK));
	if ((tdr & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC) {
		SOFLOG("error: DSP panic, code %08x", (unsigned)(tdd & SOF_IPC_MSG_MASK));
		return false;
	}

	for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
		words[i] = dsp32(SOF_DSP_MBOX_UPLINK_OFFSET + (UInt32)i * 4);

	/* cnl_ipc_host_done(): clear BUSY (write 1), then signal DONE */
	dspUpdate32(SOF_CNL_HIPCTDR, SOF_IPC_BUSY, SOF_IPC_BUSY);
	dspUpdate32(SOF_CNL_HIPCTDA, SOF_IPC_DONE, SOF_IPC_DONE);

	if ((tdr & SOF_IPC_MSG_MASK) != SOF_IPC_FW_READY || r->hdrCmd != SOF_IPC_FW_READY) {
		SOFLOG("error: unexpected first message (doorbell %08x, mailbox cmd %08x size %u)",
		       (unsigned)tdr, (unsigned)r->hdrCmd, (unsigned)r->hdrSize);
		return false;
	}
	memcpy(tag, r->version.tag, 6);
	tag[6] = 0;
	SOFLOG("FW_READY: version %u.%u.%u build %u tag '%s' ABI %u:%u:%u",
	       r->version.major, r->version.minor, r->version.micro, r->version.build, tag,
	       (unsigned)((r->version.abiVersion >> 24) & 0xFF), (unsigned)((r->version.abiVersion >> 12) & 0xFFF),
	       (unsigned)(r->version.abiVersion & 0xFFF));
	SOFLOG("FW_READY: dspbox 0x%x/%u hostbox 0x%x/%u flags 0x%llx",
	       (unsigned)r->dspboxOffset, (unsigned)r->dspboxSize, (unsigned)r->hostboxOffset,
	       (unsigned)r->hostboxSize, (unsigned long long)r->flags);
	return true;
}

/* hda_dsp_cl_boot_firmware_iccmax() + wait for FW_READY + post_fw_run */
bool VoodooHDASof::bootFirmware()
{
	const UInt8 *fw = gSofFirmware;
	UInt32 fwSize = gSofFirmwareSize, payload = 0, v;
	int attempt;

	/* ext manifest: payload starts after it (sof_ipc3_fw_parse_ext_man) */
	if (fwSize > 16 && *(const UInt32 *)fw == SOF_EXT_MAN_MAGIC)
		payload = *(const UInt32 *)(fw + 4);
	if (!payload || payload >= fwSize || (payload & 3)) {
		SOFLOG("error: bad firmware image (size %u, ext manifest %u)", (unsigned)fwSize, (unsigned)payload);
		return false;
	}
	SOFLOG("firmware: %u bytes, ext manifest %u, code loader payload %u bytes",
	       (unsigned)fwSize, (unsigned)payload, (unsigned)(fwSize - payload));

	if (!mPpCap) {
		SOFLOG("error: no PP capability");
		return false;
	}
	if (!pickStreams())
		return false;
	if (dsp32(SOF_DSP_REG_ADSPCS) & SOF_ADSPCS_CPA(SOF_CORE0)) {
		SOFLOG("error: core 0 already powered (ADSPCS %08x), not touching it",
		       (unsigned)dsp32(SOF_DSP_REG_ADSPCS));
		return false;
	}

	/* pre_fw_run */
	clockPowerGating(false);

	/* ICCMAX workaround stream */
	mSavedLtrp = hda8(SOF_VS_LTRP);
	if (!iccmaxStart()) {
		abortBoot("ICCMAX stream");
		return false;
	}

	if (!loaderPrepare(fw + payload, fwSize - payload)) {
		abortBoot("code loader stream");
		return false;
	}

	for (attempt = 1; attempt <= SOF_FW_BOOT_ATTEMPTS; attempt++) {
		SOFLOG("core 0 / ROM init, attempt %d", attempt);
		if (clDspInit(mLoader.tag))
			break;
	}
	if (attempt > SOF_FW_BOOT_ATTEMPTS) {
		abortBoot("ROM init failed");
		return false;
	}

	if (mStep < kSofStepFw) {
		SOFLOG("step %u reached (ROM init OK), powering DSP down again", (unsigned)mStep);
		dumpState("ROM init");
		abortBoot("voodoosofstep");
		dumpState("after power down");
		return true;
	}

	/* hda_cl_copy_fw() */
	SOFLOG("code loader DMA starting");
	loaderTrigger(true);
	bool entered = pollDsp(SOF_SRAM_ROM_STATUS, 0xFFFFFFU, SOF_FSR_FW_ENTERED, SOF_BASEFW_TIMEOUT_US, &v);
	UInt32 lpib = hda32(mLoader.sd + SOF_SD_LPIB);
	bool stopped = loaderTrigger(false);
	if (!entered) {
		SOFLOG("error: FW_ENTERED timeout, ROM_STATUS %08x ROM_ERROR %08x LPIB %08x", (unsigned)v,
		       (unsigned)dsp32(SOF_SRAM_ROM_ERROR), (unsigned)lpib);
		dumpState("FW_ENTERED timeout");
		abortBoot("firmware download");
		return false;
	}
	SOFLOG("FW_ENTERED: ROM_STATUS %08x LPIB %08x, DMA %s", (unsigned)v, (unsigned)lpib,
	       stopped ? "stopped" : "stop FAILED");

	loaderCleanup();
	iccmaxCleanup();

	if (!waitFwReady()) {
		dumpState("FW_READY failed");
		abortBoot("FW_READY");
		return false;
	}
	mFwReady = true;

	/* post_fw_run */
	clockPowerGating(true);
	SOFLOG("firmware boot complete");
	dumpState("FW_READY");
	return true;
}

/* stop path: reset + power down core 0, as on a failed boot */
void VoodooHDASof::powerDownDsp()
{
	if (!mDsp)
		return;
	loaderCleanup();
	iccmaxCleanup();
	if (mDspPowered || mFwReady) {
		coreResetPowerDown(SOF_CORE0);
		mFwReady = false;
	}
	if (mGatingDisabled)
		clockPowerGating(true);
}

/* ============================================================================ */
/* lifecycle                                                                    */
/* ============================================================================ */

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

	uint64_t t0, t1, ns;
	clock_get_uptime(&t0);
	bool ok = bootFirmware();
	clock_get_uptime(&t1);
	absolutetime_to_nanoseconds(t1 - t0, &ns);
	SOFLOG("boot sequence %s after %llu ms", ok ? "finished" : "FAILED", (unsigned long long)(ns / 1000000));
	return ok;
}

void VoodooHDASof::stop()
{
	SOFDBG("stop");
	powerDownDsp();
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
