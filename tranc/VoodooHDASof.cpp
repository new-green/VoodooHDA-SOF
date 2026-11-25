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

static void sofFreeDma(DmaMemory *d);

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
	mLoader.index = mIccmax.index = mDmic.index = -1;
	mVolLevel = SOF_VOL_DEFAULT;
	mLock = IOLockAlloc();
	if (!mLock)
		return false;
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

/* Config space as left by firmware (captured before VoodooHDA's writes) vs now. */
void VoodooHDASof::dumpConfig()
{
	if (!mDev->mOrigPciCfgValid) {
		SOFLOG("cfg: original config space not captured");
		return;
	}
	const UInt32 *o = mDev->mOrigPciCfg;
	for (int i = 0x40 / 4; i < 64; i += 8)
		SOFLOG("cfg fw  %02x: %08x %08x %08x %08x %08x %08x %08x %08x", i * 4,
		       (unsigned)o[i], (unsigned)o[i + 1], (unsigned)o[i + 2], (unsigned)o[i + 3],
		       (unsigned)o[i + 4], (unsigned)o[i + 5], (unsigned)o[i + 6], (unsigned)o[i + 7]);
	for (int i = 0; i < 64; i++) {
		UInt32 now = mPci->configRead32((UInt8)(i * 4));
		if (now != o[i])
			SOFLOG("cfg diff %02x: fw %08x -> now %08x", i * 4, (unsigned)o[i], (unsigned)now);
	}
}

void VoodooHDASof::dumpState(const char *when)
{
	SOFLOG("---- state (%s) ----", when);
	SOFLOG("pci: cmd %04x class %08x PGCTL(0x44) %08x CGCTL(0x48) %08x",
	       mPci->configRead16(kIOPCIConfigCommand), (unsigned)mPci->configRead32(kIOPCIConfigRevisionID),
	       (unsigned)mPci->configRead32(SOF_PCI_PGCTL), (unsigned)mPci->configRead32(SOF_PCI_CGCTL));
	SOFLOG("hda: GCAP %04x GCTL %08x INTCTL %08x INTSTS %08x EM2 %08x LTRP %02x D0I3C %02x",
	       (unsigned)hda16(0x00), (unsigned)hda32(0x08), (unsigned)hda32(0x20), (unsigned)hda32(0x24),
	       (unsigned)hda32(SOF_VS_EM2), (unsigned)hda8(SOF_VS_LTRP), (unsigned)hda8(SOF_VS_D0I3C));
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
			SOFLOG("sram: ext rom status %08x %08x %08x %08x %08x %08x %08x %08x",
			       (unsigned)dsp32(SOF_DSP_MBOX_OFFSET + 0x00), (unsigned)dsp32(SOF_DSP_MBOX_OFFSET + 0x04),
			       (unsigned)dsp32(SOF_DSP_MBOX_OFFSET + 0x08), (unsigned)dsp32(SOF_DSP_MBOX_OFFSET + 0x0C),
			       (unsigned)dsp32(SOF_DSP_MBOX_OFFSET + 0x10), (unsigned)dsp32(SOF_DSP_MBOX_OFFSET + 0x14),
			       (unsigned)dsp32(SOF_DSP_MBOX_OFFSET + 0x18), (unsigned)dsp32(SOF_DSP_MBOX_OFFSET + 0x1C));
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

/*
 * linuxStreams=false: highest free capture/playback SDs (never used by VoodooHDA).
 * linuxStreams=true: exactly what Linux picks on a cold boot — first capture SD (ICCMAX) and
 * first playback SD (code loader), both tag 1. These belong to VoodooHDA, so they are only
 * borrowed while idle; VoodooHDA resets and fully reprograms an SD on every channel start.
 */
bool VoodooHDASof::pickStreams(bool linuxStreams)
{
	int iss = mDev->mInStreamsSup, oss = mDev->mOutStreamsSup;

	if (iss < 2 || oss < 2) {
		SOFLOG("error: not enough streams (ISS %d OSS %d)", iss, oss);
		return false;
	}
	mIccmax.playback = false;
	mLoader.playback = true;
	if (linuxStreams) {
		mIccmax.index = 0;
		mIccmax.tag = 1;
		mLoader.index = iss;
		mLoader.tag = 1;
	} else {
		mIccmax.index = iss - 1;
		mIccmax.tag = iss;
		mLoader.index = iss + oss - 1;
		mLoader.tag = oss;
	}
	mIccmax.sd = SOF_SD_OFFSET(mIccmax.index);
	mLoader.sd = SOF_SD_OFFSET(mLoader.index);

	for (int i = 0; i < mDev->mNumChannels; i++) {
		int sd = mDev->mChannels[i].off >> 5;
		if (sd != mIccmax.index && sd != mLoader.index)
			continue;
		if (!linuxStreams) {
			SOFLOG("error: SD%d is used by VoodooHDA channel %d", sd, i);
			mIccmax.index = mLoader.index = -1;
			return false;
		}
		UInt32 ctl = hda32(SOF_SD_OFFSET(sd));
		if (ctl & SOF_SD_CTL_DMA_START) {
			SOFLOG("error: SD%d (VoodooHDA channel %d) is running (ctl %08x), not borrowing it",
			       sd, i, (unsigned)ctl);
			mIccmax.index = mLoader.index = -1;
			return false;
		}
		SOFLOG("borrowing idle SD%d from VoodooHDA channel %d (ctl %08x)", sd, i, (unsigned)ctl);
	}
	SOFLOG("streams: code loader SD%d tag %d, ICCMAX SD%d tag %d", mLoader.index, mLoader.tag,
	       mIccmax.index, mIccmax.tag);
	return true;
}

/*
 * Physically contiguous, uncached DMA memory (same as VoodooHDADevice::allocateDmaMemory, so
 * freeDmaMemory() releases it), optionally below 4 GB. Verifies contiguity.
 */
DmaMemory *VoodooHDASof::allocDma(UInt32 size, const char *what)
{
	mach_vm_address_t mask = mLow4G ? 0xFFFFFF80ULL : ~0x7FULL;
	IOByteCount seg = 0;
	IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		kIOMemoryPhysicallyContiguous | kIODirectionInOut | kIOMapInhibitCache, size, mask);

	if (!md)
		return NULL;
	if (md->prepare() != kIOReturnSuccess) {
		md->release();
		return NULL;
	}
	UInt64 phys = md->getPhysicalSegment(0, &seg, 0);
	if (!phys || seg < size) {
		SOFLOG("error: %s not contiguous (phys 0x%llx seg 0x%llx size 0x%x)", what,
		       (unsigned long long)phys, (unsigned long long)seg, (unsigned)size);
		md->complete();
		md->release();
		return NULL;
	}
	DmaMemory *d = new DmaMemory;
	d->description = what;
	d->md = md;
	d->size = size;
	d->physAddr = phys;
	d->virtAddr = reinterpret_cast<IOVirtualAddress>(md->getBytesNoCopy());
	bzero((void *)d->virtAddr, size);
	return d;
}

bool VoodooHDASof::streamAlloc(SofStream *s, UInt32 size, const char *what)
{
	s->size = size;
	s->frags = 0;
	s->buf = allocDma(size, what);
	s->bdl = allocDma(SOF_BDL_SIZE, "SOF BDL");
	if (!s->buf || !s->bdl) {
		SOFLOG("error: DMA alloc failed for %s (%u bytes)", what, (unsigned)size);
		streamFree(s);
		return false;
	}
	SOFLOG("%s: buf phys 0x%llx size 0x%x, bdl phys 0x%llx", what,
	       (unsigned long long)s->buf->physAddr, (unsigned)size, (unsigned long long)s->bdl->physAddr);
	return true;
}

void VoodooHDASof::streamFree(SofStream *s)
{
	if (s->buf) {
		sofFreeDma(s->buf);
		s->buf = NULL;
	}
	if (s->bdl) {
		sofFreeDma(s->bdl);
		s->bdl = NULL;
	}
}

/*
 * hda_dsp_stream_setup_bdl(): the buffer is physically contiguous; split it into
 * two equal periods exactly like Linux does for a single contiguous area ("periods: 2").
 * IOC per period only in the Linux-identical variant (no_ipc_position=1 -> ioc=1).
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
		bdl[i].ioc = mLinuxIntBits ? 1 : 0;
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
	/* SD interrupt enables only in the Linux-identical variant; INTCTL stays off either way */
	if (mLinuxIntBits)
		hdaUpdate32(s->sd, SOF_SD_INT_MASK, SOF_SD_INT_MASK);

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
		UInt32 bits = SOF_SD_CTL_DMA_START | (mLinuxIntBits ? SOF_SD_INT_MASK : 0);
		hdaUpdate32(s->sd, bits, bits);
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

void VoodooHDASof::restoreDevc()
{
	if (!mDevcChanged)
		return;
	mPci->configWrite16(SOF_PCI_DEVC, mSavedDevc);
	mDevcChanged = false;
	SOFLOG("DEVC restored to VoodooHDA value %04x", (unsigned)mPci->configRead16(SOF_PCI_DEVC));
}

void VoodooHDASof::abortBoot(const char *why)
{
	SOFLOG("boot aborted: %s", why);
	restoreDevc();
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

	/* ipc3_fw_ready(): the ext data (mailbox windows) is parsed before the message is acked */
	if (mStep >= kSofStepIpc && !parseExtData()) {
		UInt32 b = SOF_DSP_MBOX_UPLINK_OFFSET + (UInt32)sizeof(SofIpcFwReady);
		for (UInt32 i = 0; i < 64; i += 8)
			SOFLOG("ext raw +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)(i * 4),
			       (unsigned)dsp32(b + i * 4), (unsigned)dsp32(b + i * 4 + 4), (unsigned)dsp32(b + i * 4 + 8),
			       (unsigned)dsp32(b + i * 4 + 12), (unsigned)dsp32(b + i * 4 + 16), (unsigned)dsp32(b + i * 4 + 20),
			       (unsigned)dsp32(b + i * 4 + 24), (unsigned)dsp32(b + i * 4 + 28));
	}

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

/*
 * Wait for FW_ENTERED while the code loader DMA runs, logging every change of ROM status,
 * SD status and the host DMA position (PPHC). Gives up early when the ROM halts.
 */
bool VoodooHDASof::traceCopy(UInt32 *lastFsr)
{
	UInt32 pphc = mPpCap + 0x10 + 0x10 * (UInt32)mLoader.index;
	UInt32 fsr = 0, prevFsr = 0xFFFFFFFFU, prevSts = 0xFFFFFFFFU, prevLdp = 0xFFFFFFFFU;
	uint64_t t0, t, ns;
	int lines = 0;
	bool ok = false;

	clock_get_uptime(&t0);
	for (;;) {
		fsr = dsp32(SOF_SRAM_ROM_STATUS);
		UInt32 sts = hda8(mLoader.sd + SOF_SD_STS);
		UInt32 ldp = hda32(pphc + 0x8);
		clock_get_uptime(&t);
		absolutetime_to_nanoseconds(t - t0, &ns);
		UInt32 us = (UInt32)(ns / 1000);

		bool changed = fsr != prevFsr || sts != prevSts;
		bool ldpMoved = ldp != prevLdp && (us < 2000 || (ldp >> 15) != (prevLdp >> 15));
		if ((changed || ldpMoved) && lines < 60) {
			SOFLOG("  +%6u us: ROM %08x err %08x | SD%d sts %02x ctl %08x | PPHC llp %08x ldp %08x | SPIB %08x",
			       (unsigned)us, (unsigned)fsr, (unsigned)dsp32(SOF_SRAM_ROM_ERROR), mLoader.index,
			       (unsigned)sts, (unsigned)hda32(mLoader.sd), (unsigned)hda32(pphc),
			       (unsigned)ldp, mSpbCap ? (unsigned)hda32(mSpbCap + SOF_SPB_SPIB(mLoader.index)) : 0U);
			lines++;
		}
		prevFsr = fsr;
		prevSts = sts;
		prevLdp = ldp;

		if (SOF_FSR_STATE(fsr) == SOF_FSR_FW_ENTERED) {
			ok = true;
			break;
		}
		if (fsr & 0x80000000U) {	/* FSR_HALTED */
			SOFLOG("  ROM halted after %u us", (unsigned)us);
			break;
		}
		if (us >= SOF_BASEFW_TIMEOUT_US) {
			SOFLOG("  timeout after %u us", (unsigned)us);
			break;
		}
		if (us < 200000)
			IODelay(50);
		else
			IOSleep(1);
	}
	if (lastFsr)
		*lastFsr = fsr;
	return ok;
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
	if (dsp32(SOF_DSP_REG_ADSPCS) & SOF_ADSPCS_CPA(SOF_CORE0)) {
		SOFLOG("error: core 0 already powered (ADSPCS %08x), not touching it",
		       (unsigned)dsp32(SOF_DSP_REG_ADSPCS));
		return false;
	}

	/*
	 * VoodooHDA clears 0x44[2:0] ("TCSEL -> TC0"), but on SKL+ 0x44 is PGCTL (Linux sets
	 * AZX_DCAPS_NO_TCSEL there and SOF never writes it). Give the DSP the firmware's value back.
	 */
	if (mDev->mOrigPciCfgValid) {
		UInt32 fw44 = mDev->mOrigPciCfg[SOF_PCI_PGCTL / 4], now44 = mPci->configRead32(SOF_PCI_PGCTL);
		if (fw44 != now44) {
			mPci->configWrite32(SOF_PCI_PGCTL, fw44);
			SOFLOG("PGCTL restored to firmware value: %08x -> %08x (reads %08x)", (unsigned)now44,
			       (unsigned)fw44, (unsigned)mPci->configRead32(SOF_PCI_PGCTL));
		} else {
			SOFLOG("PGCTL unchanged by VoodooHDA (%08x)", (unsigned)now44);
		}
	}

	/*
	 * VoodooHDA clears DEVC (0x78) bit 11 NSNPEN to force snooped DMA; Linux SOF leaves the
	 * firmware value (NSNPEN=1 here). Give the DSP the firmware value for the boot window only;
	 * VoodooHDA's value is put back in restoreDevc().
	 */
	mSavedDevc = mPci->configRead16(SOF_PCI_DEVC);
	mDevcChanged = false;
	if (mDev->mOrigPciCfgValid) {
		UInt16 fwDevc = (UInt16)mDev->mOrigPciCfg[SOF_PCI_DEVC / 4];
		UInt16 want = (UInt16)((mSavedDevc & ~SOF_PCI_DEVC_NSNPEN) | (fwDevc & SOF_PCI_DEVC_NSNPEN));
		if (want != mSavedDevc) {
			mPci->configWrite16(SOF_PCI_DEVC, want);
			mDevcChanged = true;
			SOFLOG("DEVC NSNPEN set to firmware value for DSP boot: %04x -> %04x", (unsigned)mSavedDevc,
			       (unsigned)mPci->configRead16(SOF_PCI_DEVC));
		}
	}

	/* pre_fw_run */
	clockPowerGating(false);
	mSavedLtrp = hda8(SOF_VS_LTRP);

	/*
	 * d5 showed: with SD15/SD6 the ROM fetches 32 KB, loads both manifests, then halts
	 * (0x80000003); buffers below 4 GB and Linux SD int bits/IOC made no difference.
	 * Variant 1: Linux's exact streams (loader SD(ISS) tag 1, ICCMAX SD0 tag 1).
	 * Variant 2: same streams, without the ICCMAX stream.
	 */
	bool entered = false;
	for (int variant = 1; variant <= 1 && !entered; variant++) {
		bool useIccmax = true;
		mLow4G = false;
		mLinuxIntBits = true;
		SOFLOG("==== variant %d: Linux streams%s ====", variant, useIccmax ? " + ICCMAX" : ", no ICCMAX");
		if (!pickStreams(true)) {
			abortBoot("no streams");
			return false;
		}

		/* ICCMAX workaround stream */
		if (useIccmax && !iccmaxStart()) {
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
		entered = traceCopy(&v);
		bool stopped = loaderTrigger(false);
		if (!entered) {
			SOFLOG("error: no FW_ENTERED, ROM_STATUS %08x ROM_ERROR %08x", (unsigned)v,
			       (unsigned)dsp32(SOF_SRAM_ROM_ERROR));
			dumpState("FW download failed");
			coreResetPowerDown(SOF_CORE0);
			loaderCleanup();
			iccmaxCleanup();
			continue;
		}
		SOFLOG("FW_ENTERED: ROM_STATUS %08x, DMA %s", (unsigned)v, stopped ? "stopped" : "stop FAILED");
	}
	if (!entered) {
		abortBoot("firmware download (both variants)");
		return false;
	}

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
	restoreDevc();
	SOFLOG("firmware boot complete");
	dumpState("FW_READY");
	return true;
}

/* stop path: reset + power down core 0, as on a failed boot */
void VoodooHDASof::powerDownDsp()
{
	if (!mDsp)
		return;
	if (mFwReady && mDspBoxSize) {
		dmicStop();
		teardownTopology();
	} else {
		dmicHwStop();
	}
	if (mNsnpFallback) {
		mPci->configWrite16(SOF_PCI_DEVC, mSavedDevc);
		mNsnpFallback = false;
	}
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
/* M3: IPC3                                                                     */
/* ============================================================================ */

extern "C" {
extern const SofReplayMsg gSofReplay[];
extern const unsigned int gSofReplayCount;
}

void VoodooHDASof::mboxWrite(UInt32 off, const void *buf, UInt32 len)
{
	const UInt8 *b = (const UInt8 *)buf;
	for (UInt32 i = 0; i < len; i += 4) {
		UInt32 w = 0;
		memcpy(&w, b + i, (len - i) >= 4 ? 4 : len - i);
		dspW32(off + i, w);
	}
}

void VoodooHDASof::mboxRead(UInt32 off, void *buf, UInt32 len)
{
	UInt8 *b = (UInt8 *)buf;
	for (UInt32 i = 0; i < len; i += 4) {
		UInt32 w = dsp32(off + i);
		memcpy(b + i, &w, (len - i) >= 4 ? 4 : len - i);
	}
}

/* ipc3_fw_parse_ext_data() + ipc3_get_windows(): find host_box / dsp_box */
/*
 * sof_ipc3_fw_parse_ext_man(): SOF 2.x carries the window list in the firmware file's
 * extended manifest (element SOF_EXT_MAN_ELEM_WINDOW), not in the FW_READY mailbox.
 */
bool VoodooHDASof::parseExtManWindows()
{
	const UInt8 *fw = gSofFirmware;
	UInt32 full = *(const UInt32 *)(fw + 4), hsz = *(const UInt32 *)(fw + 8);

	if (*(const UInt32 *)fw != SOF_EXT_MAN_MAGIC || full > gSofFirmwareSize || hsz < 16 || hsz >= full)
		return false;
	for (UInt32 off = hsz; off + 8 <= full; ) {
		UInt32 type = *(const UInt32 *)(fw + off), size = *(const UInt32 *)(fw + off + 4);
		if (size < 8 || off + size > full)
			break;
		if (type == SOF_EXT_MAN_ELEM_WINDOW && size >= 8 + 16) {
			const UInt8 *w = fw + off + 8;		/* struct sof_ipc_window */
			UInt32 num = *(const UInt32 *)(w + 12);
			SOFLOG("ext manifest: %u windows", (unsigned)num);
			for (UInt32 i = 0; i < num && i < 16 && 8 + 16 + (i + 1) * 24 <= size; i++) {
				const UInt32 *e = (const UInt32 *)(w + 16 + i * 24);	/* hdr, type, id, flags, size, offset */
				UInt32 where = SOF_SRAM_WINDOW_OFFSET(e[2]) + e[5];
				SOFLOG("  window: type %u id %u size 0x%x offset 0x%x -> BAR4 0x%x", (unsigned)e[1],
				       (unsigned)e[2], (unsigned)e[4], (unsigned)e[5], (unsigned)where);
				if (e[1] == SOF_IPC_REGION_DOWNBOX) { mHostBox = where; mHostBoxSize = e[4]; }
				if (e[1] == SOF_IPC_REGION_UPBOX)   { mDspBox = where;  mDspBoxSize = e[4]; }
			}
		}
		off += size;
	}
	if (!mHostBoxSize || !mDspBoxSize || mHostBox + mHostBoxSize > mDspLen || mDspBox + mDspBoxSize > mDspLen) {
		mHostBoxSize = mDspBoxSize = 0;
		return false;
	}
	SOFLOG("mailbox (ext manifest): host_box 0x%x (0x%x), dsp_box 0x%x (0x%x)", (unsigned)mHostBox,
	       (unsigned)mHostBoxSize, (unsigned)mDspBox, (unsigned)mDspBoxSize);
	return true;
}

bool VoodooHDASof::parseExtData()
{
	if (parseExtManWindows())
		return true;
	if (parseExtDataAt(SOF_DSP_MBOX_UPLINK_OFFSET + (UInt32)sizeof(SofIpcFwReady)))
		return true;

	/* not where Linux expects it: find every ext header (cmd == FW_READY) in the SRAM windows */
	SOFLOG("ext data not after fw_ready (fw_ready hdr.size %u), scanning SRAM windows",
	       (unsigned)dsp32(SOF_DSP_MBOX_UPLINK_OFFSET));
	int hits = 0;
	for (UInt32 off = SOF_DSP_MBOX_OFFSET; off + 32 <= mDspLen && hits < 16; off += 4) {
		if (dsp32(off + 4) != SOF_IPC_FW_READY)
			continue;
		UInt32 sz = dsp32(off), type = dsp32(off + 8);
		SOFLOG("  hit @0x%05x: size %u type %u | %08x %08x %08x %08x %08x", (unsigned)off, (unsigned)sz,
		       (unsigned)type, (unsigned)dsp32(off + 12), (unsigned)dsp32(off + 16), (unsigned)dsp32(off + 20),
		       (unsigned)dsp32(off + 24), (unsigned)dsp32(off + 28));
		hits++;
		if (!mHostBoxSize && type == SOF_IPC_EXT_WINDOW && sz >= 16 && sz <= 4096)
			parseExtDataAt(off);
	}
	SOFLOG("scan done: %d hits", hits);
	return mHostBoxSize && mDspBoxSize;
}

bool VoodooHDASof::parseExtDataAt(UInt32 off)
{
	UInt32 hdr[3];

	for (int n = 0; n < 16; n++) {
		mboxRead(off, hdr, sizeof hdr);		/* size, cmd, type */
		if (hdr[1] != SOF_IPC_FW_READY || hdr[0] < 12 || hdr[0] > 4096)
			break;
		SOFLOG("ext data: type %u size %u", (unsigned)hdr[2], (unsigned)hdr[0]);
		if (hdr[2] == SOF_IPC_EXT_WINDOW) {
			UInt32 num = dsp32(off + 12);
			for (UInt32 i = 0; i < num && i < 16 && 16 + (i + 1) * 24 <= hdr[0]; i++) {
				UInt32 e[6];	/* hdr.size, type, id, flags, size, offset */
				mboxRead(off + 16 + i * 24, e, sizeof e);
				UInt32 where = SOF_SRAM_WINDOW_OFFSET(e[2]) + e[5];
				SOFLOG("  window: type %u id %u flags %x size 0x%x offset 0x%x -> BAR4 0x%x",
				       (unsigned)e[1], (unsigned)e[2], (unsigned)e[3], (unsigned)e[4], (unsigned)e[5],
				       (unsigned)where);
				if (e[1] == SOF_IPC_REGION_DOWNBOX) { mHostBox = where; mHostBoxSize = e[4]; }
				if (e[1] == SOF_IPC_REGION_UPBOX)   { mDspBox = where;  mDspBoxSize = e[4]; }
			}
		}
		off += hdr[0];
	}
	if (!mHostBoxSize || !mDspBoxSize || mHostBox + mHostBoxSize > mDspLen || mDspBox + mDspBoxSize > mDspLen) {
		SOFLOG("error: no usable mailbox (host 0x%x/0x%x dsp 0x%x/0x%x)", (unsigned)mHostBox,
		       (unsigned)mHostBoxSize, (unsigned)mDspBox, (unsigned)mDspBoxSize);
		return false;
	}
	SOFLOG("mailbox: host_box 0x%x (0x%x), dsp_box 0x%x (0x%x)", (unsigned)mHostBox, (unsigned)mHostBoxSize,
	       (unsigned)mDspBox, (unsigned)mDspBoxSize);
	return true;
}

/* DSP-initiated message (HIPCTDR BUSY): log it and ack (cnl_ipc_host_done) */
void VoodooHDASof::serviceNotification()
{
	UInt32 tdr = dsp32(SOF_CNL_HIPCTDR);

	if (!(tdr & SOF_IPC_BUSY))
		return;
	UInt32 tdd = dsp32(SOF_CNL_HIPCTDD);
	UInt32 hdr[2] = { 0, 0 };
	if (mDspBoxSize)
		mboxRead(mDspBox, hdr, sizeof hdr);
	if ((tdr & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC)
		SOFLOG("error: DSP PANIC doorbell %08x code %08x", (unsigned)tdr, (unsigned)tdd);
	else if (mNotifyCount < 20)
		SOFLOG("notification: doorbell %08x ext %08x, msg cmd %08x size %u", (unsigned)tdr, (unsigned)tdd,
		       (unsigned)hdr[1], (unsigned)hdr[0]);
	mNotifyCount++;
	dspUpdate32(SOF_CNL_HIPCTDR, SOF_IPC_BUSY, SOF_IPC_BUSY);
	dspUpdate32(SOF_CNL_HIPCTDA, SOF_IPC_DONE, SOF_IPC_DONE);
}

/*
 * cnl_ipc_send_msg() (mailbox path) + polled reply (hda_dsp_ipc_get_reply, cnl_ipc_dsp_done).
 * size includes the 8-byte header; payload is size-8 bytes.
 */
bool VoodooHDASof::ipcTx(UInt32 cmd, const UInt8 *payload, UInt32 size, void *reply, UInt32 replyMax)
{
	UInt32 msg[SOF_IPC_MSG_MAX_SIZE / 4];
	UInt32 rep[SOF_IPC_MSG_MAX_SIZE / 4];
	UInt32 v, waited = 0;

	if (size < 8 || size > sizeof msg || size > mHostBoxSize) {
		SOFLOG("error: ipc %08x bad size %u", (unsigned)cmd, (unsigned)size);
		return false;
	}
	if (dsp32(SOF_CNL_HIPCIDR) & SOF_IPC_BUSY)	/* Linux does not check this; just note it */
		SOFLOG("warning: ipc %08x: HIPCIDR still busy (%08x)", (unsigned)cmd, (unsigned)dsp32(SOF_CNL_HIPCIDR));
	bzero(msg, sizeof msg);
	msg[0] = size;
	msg[1] = cmd;
	if (size > 8)
		memcpy(&msg[2], payload, size - 8);
	mboxWrite(mHostBox, msg, (size + 3) & ~3U);
	dspW32(SOF_CNL_HIPCIDR, SOF_IPC_BUSY);

	for (;;) {
		v = dsp32(SOF_CNL_HIPCIDA);
		if (v & SOF_IPC_DONE)
			break;
		serviceNotification();
		if (waited >= SOF_IPC_TIMEOUT_US) {
			SOFLOG("error: ipc %08x timeout (IDR %08x IDA %08x TDR %08x)", (unsigned)cmd,
			       (unsigned)dsp32(SOF_CNL_HIPCIDR), (unsigned)v, (unsigned)dsp32(SOF_CNL_HIPCTDR));
			return false;
		}
		if (waited < 2000) { IODelay(20); waited += 20; } else { IOSleep(1); waited += 1000; }
	}

	UInt32 rsize = dsp32(mDspBox);
	if (rsize < 12 || rsize > sizeof rep || rsize > mDspBoxSize)
		rsize = 12;
	mboxRead(mDspBox, rep, rsize);
	dspUpdate32(SOF_CNL_HIPCIDA, SOF_IPC_DONE, SOF_IPC_DONE);	/* cnl_ipc_dsp_done */
	if (reply && replyMax)
		memcpy(reply, rep, rsize < replyMax ? rsize : replyMax);

	SInt32 err = (SInt32)rep[2];
	if (mDbg || err)
		SOFLOG("ipc %08x (%u B) -> reply cmd %08x size %u error %d (%u us)", (unsigned)cmd, (unsigned)size,
		       (unsigned)rep[1], (unsigned)rep[0], (int)err, (unsigned)waited);
	return err == 0;
}

bool VoodooHDASof::replayOne(int idx, const UInt8 *payloadOverride)
{
	if (idx < 0 || (unsigned)idx >= gSofReplayCount)
		return false;
	const SofReplayMsg *m = &gSofReplay[idx];
	bool ok = ipcTx(m->cmd, payloadOverride ? payloadOverride : m->payload, m->size, NULL, 0);
	SOFLOG("  #%02d %-13s %08x %3u B: %s", idx, m->name, (unsigned)m->cmd, (unsigned)m->size, ok ? "ok" : "FAILED");
	return ok;
}

bool VoodooHDASof::replay(int from, int to)
{
	for (int i = from; i <= to; i++)
		if (!replayOne(i))
			return false;
	return true;
}

/* free the DMIC pipeline: COMP_FREE/BUFFER_FREE/PIPE_FREE (#24-31) */
void VoodooHDASof::teardownTopology()
{
	if (!mTopology)
		return;
	SOFLOG("freeing topology (#24-31)");
	for (int i = 24; i <= 31; i++)
		replayOne(i);
	mTopology = false;
}

/*
 * Q8.16 linear gain per "Capture Volume" step: round(65536 * 10^((step - 50) / 20)), step 0 = mute
 * (Linux set_up_volume_table() with the topology TLV: min -50 dB, step 1 dB, mute).
 */
static const UInt32 kSofVolGain[SOF_VOL_STEPS + 1] = {
	0x0, 0xe9, 0x105, 0x125, 0x148, 0x171, 0x19e, 0x1d0, 0x209, 0x248, 0x28f, 0x2df, 0x339, 0x39e,
	0x40f, 0x48d, 0x51c, 0x5bb, 0x66e, 0x737, 0x818, 0x915, 0xa31, 0xb6f, 0xcd5, 0xe65, 0x1027, 0x1220,
	0x1456, 0x16d1, 0x199a, 0x1cb9, 0x203a, 0x2429, 0x2893, 0x2d86, 0x3314, 0x3950, 0x404e, 0x4827,
	0x50f4, 0x5ad5, 0x65ea, 0x725a, 0x804e, 0x8ff6, 0xa186, 0xb53c, 0xcb59, 0xe429, 0x10000, 0x11f3d,
	0x14249, 0x1699c, 0x195bc, 0x1c73d, 0x1feca, 0x23d1d, 0x2830b, 0x2d182, 0x3298b, 0x38c53, 0x3fb28,
	0x47783, 0x5030a, 0x59f98, 0x64f40, 0x71457, 0x7f17b, 0x8e99a, 0xa0000
};

/* SET_VALUE on the volume component: replay message with both channel values replaced. Caller holds mLock. */
bool VoodooHDASof::sendCtrl(int replayIdx, UInt32 value, const char *what)
{
	const SofReplayMsg *m = &gSofReplay[replayIdx];
	UInt8 pl[SOF_IPC_MSG_MAX_SIZE];

	if (!mDsp || !mFwReady || !mTopology)
		return false;
	if ((unsigned)replayIdx >= gSofReplayCount || m->size - 8 < SOF_CTRL_CHAN_VALUE(1) + 4 ||
	    m->size - 8 > sizeof pl) {
		SOFLOG("error: replay #%d is not a 2-channel SET_VALUE", replayIdx);
		return false;
	}
	memcpy(pl, m->payload, m->size - 8);
	memcpy(pl + SOF_CTRL_CHAN_VALUE(0), &value, 4);
	memcpy(pl + SOF_CTRL_CHAN_VALUE(1), &value, 4);
	bool ok = ipcTx(m->cmd, pl, m->size, NULL, 0);
	SOFDBG("%s %08x: %s", what, (unsigned)value, ok ? "ok" : "FAILED");
	if (!ok)
		SOFLOG("error: %s %08x failed", what, (unsigned)value);
	return ok;
}

/* ============================================================================ */
/* M3/M4: DMIC capture                                                          */
/* ============================================================================ */

/* freeDmaMemory() equivalent that does not need the (possibly gone) VoodooHDADevice */
static void sofFreeDma(DmaMemory *d)
{
	if (!d)
		return;
	if (d->md) {
		d->md->complete();
		d->md->release();
	}
	delete d;
}

/* one-time: 64 KB buffer + BDL + page table, all below 4 GB (32-bit phy_addr, 20-bit PFNs) */
bool VoodooHDASof::dmicAlloc()
{
	SofStream *s = &mDmic;
	int iss = mDev->mInStreamsSup;

	if (s->buf)
		return true;
	s->index = iss - 1;		/* highest capture SD, never used by VoodooHDA */
	s->tag = iss;			/* tag = capture index + 1 */
	s->playback = false;
	s->sd = SOF_SD_OFFSET(s->index);
	for (int i = 0; i < mDev->mNumChannels; i++)
		if ((mDev->mChannels[i].off >> 5) == s->index) {
			SOFLOG("error: SD%d is used by VoodooHDA", s->index);
			return false;
		}

	mLow4G = true;
	bool ok = streamAlloc(s, SOF_DMIC_BUF_SIZE, "SOF DMIC");
	mPageTable = ok ? allocDma(4096, "SOF DMIC page table") : NULL;
	mLow4G = false;
	if (!ok || !mPageTable) {
		SOFLOG("error: DMIC buffer allocation failed");
		dmicFree();
		return false;
	}

	/* snd_sof_create_page_table(): 20-bit PFNs packed 2.5 bytes each */
	UInt8 *pt = (UInt8 *)mPageTable->virtAddr;
	for (UInt32 i = 0; i < SOF_DMIC_BUF_SIZE / 4096; i++) {
		UInt32 pfn = (UInt32)((s->buf->physAddr + i * 4096ULL) >> 12);
		UInt8 *e = pt + ((5 * i) >> 1);
		UInt32 w = (i & 1) ? ((e[0] & 0xF) | (pfn << 4)) : pfn;
		e[0] = (UInt8)w; e[1] = (UInt8)(w >> 8); e[2] = (UInt8)(w >> 16); e[3] = (UInt8)(w >> 24);
	}

	/* BDL: one entry per 4 KB period, IOC set (no_ipc_position=1) */
	SofBdlEntry *bdl = (SofBdlEntry *)s->bdl->virtAddr;
	s->frags = (int)(SOF_DMIC_BUF_SIZE / SOF_DMIC_PERIOD);
	for (int i = 0; i < s->frags; i++) {
		UInt64 a = s->buf->physAddr + (UInt64)i * SOF_DMIC_PERIOD;
		bdl[i].addrLo = (UInt32)a;
		bdl[i].addrHi = (UInt32)(a >> 32);
		bdl[i].size = SOF_DMIC_PERIOD;
		bdl[i].ioc = 1;
	}
	SOFLOG("DMIC buffers: SD%d tag %d buf 0x%llx pt 0x%llx", s->index, s->tag,
	       (unsigned long long)s->buf->physAddr, (unsigned long long)mPageTable->physAddr);
	return true;
}

void VoodooHDASof::dmicFree()
{
	SofStream *s = &mDmic;

	sofFreeDma(s->buf);
	s->buf = NULL;
	sofFreeDma(s->bdl);
	s->bdl = NULL;
	sofFreeDma(mPageTable);
	mPageTable = NULL;
}

/* hda_dsp_pcm_hw_params() -> hda_dsp_stream_hw_params() for the capture host stream */
bool VoodooHDASof::dmicHwParams()
{
	SofStream *s = &mDmic;
	UInt32 mask = 1U << s->index, v;

	hdaUpdate32(mPpCap + SOF_PP_PPCTL, mask, mask);
	hdaUpdate32(s->sd, SOF_SD_INT_MASK | SOF_SD_CTL_DMA_START, 0);
	pollHda(s->sd, SOF_SD_CTL_DMA_START, 0, SOF_STREAM_RUN_TIMEOUT_US, &v);
	hdaW8(s->sd + SOF_SD_STS, SOF_SD_INT_MASK);
	if (!streamReset(s))
		return false;
	hdaW32(s->sd + SOF_SD_BDLPL, 0);
	hdaW32(s->sd + SOF_SD_BDLPU, 0);
	hdaUpdate32(s->sd, SOF_SD_INT_MASK | SOF_SD_CTL_DMA_START, 0);
	pollHda(s->sd, SOF_SD_CTL_DMA_START, 0, SOF_STREAM_RUN_TIMEOUT_US, &v);
	hdaW8(s->sd + SOF_SD_STS, SOF_SD_INT_MASK);
	hdaUpdate32(s->sd, SOF_SD_CTL_TAG_MASK, (UInt32)s->tag << SOF_SD_CTL_TAG_SHIFT);
	hdaW32(s->sd + SOF_SD_CBL, SOF_DMIC_BUF_SIZE);
	hdaW16(s->sd + SOF_SD_FORMAT, SOF_DMIC_FORMAT);
	hdaW16(s->sd + SOF_SD_LVI, (UInt16)(s->frags - 1));
	hdaW32(s->sd + SOF_SD_BDLPL, (UInt32)s->bdl->physAddr);
	hdaW32(s->sd + SOF_SD_BDLPU, (UInt32)(s->bdl->physAddr >> 32));
	hdaUpdate32(s->sd, SOF_SD_INT_MASK, SOF_SD_INT_MASK);	/* INTCTL stays off: no host irq */
	if (mSpbCap) {						/* SPIB disabled (rewinds allowed) */
		hdaUpdate32(mSpbCap + SOF_SPB_SPBFCCTL, mask, 0);
		hdaW32(mSpbCap + SOF_SPB_SPIB(s->index), 0);
	}
	return true;
}

/* host DMA stop + SD back to idle and coupled; buffers are kept */
void VoodooHDASof::dmicHwStop()
{
	SofStream *s = &mDmic;
	UInt32 v;

	if (s->index < 0)
		return;
	hdaUpdate32(s->sd, SOF_SD_CTL_DMA_START | SOF_SD_INT_MASK, 0);
	pollHda(s->sd, SOF_SD_CTL_DMA_START, 0, SOF_STREAM_RUN_TIMEOUT_US, &v);
	hdaW8(s->sd + SOF_SD_STS, SOF_SD_INT_MASK);
	hdaW32(s->sd + SOF_SD_BDLPL, 0);
	hdaW32(s->sd + SOF_SD_BDLPU, 0);
	hdaW32(s->sd, 0);
	hdaUpdate32(mPpCap + SOF_PP_PPCTL, 1U << s->index, 0);
}

/* PCM_PARAMS (#20, patched) + host DMA start + TRIG_START (#21). Caller holds mLock. */
bool VoodooHDASof::dmicStart()
{
	SofStream *s = &mDmic;

	if (mCapturing)
		return true;
	if (!mFwReady || !mTopology || !dmicAlloc())
		return false;
	bzero((void *)s->buf->virtAddr, SOF_DMIC_BUF_SIZE);
	if (!dmicHwParams()) {
		dmicHwStop();
		return false;
	}

	const SofReplayMsg *pm = &gSofReplay[20];
	UInt8 pl[SOF_IPC_MSG_MAX_SIZE];
	if (pm->cmd != SOF_IPC_STREAM_PCM_PARAMS || pm->size - 8 > sizeof pl) {
		SOFLOG("error: replay #20 is not PCM_PARAMS");
		dmicHwStop();
		return false;
	}
	memcpy(pl, pm->payload, pm->size - 8);
	UInt32 phy = (UInt32)mPageTable->physAddr, sz = SOF_DMIC_BUF_SIZE, pages = SOF_DMIC_BUF_SIZE / 4096;
	UInt16 tag = (UInt16)s->tag;
	memcpy(pl + SOF_PCMP_PHY_ADDR, &phy, 4);
	memcpy(pl + SOF_PCMP_PAGES, &pages, 4);
	memcpy(pl + SOF_PCMP_SIZE, &sz, 4);
	memcpy(pl + SOF_PCMP_STREAM_TAG, &tag, 2);
	bool ok = ipcTx(pm->cmd, pl, pm->size, NULL, 0);
	SOFLOG("  #20 PCM_PARAMS (pt 0x%x, %u pages, tag %u): %s", (unsigned)phy, (unsigned)pages,
	       (unsigned)tag, ok ? "ok" : "FAILED");
	if (!ok) {
		dmicHwStop();
		return false;
	}
	mPcmParams = true;
	/* keep the user's gain across stream setups (the topology default is what #9/#10 set) */
	if (mVolLevel != SOF_VOL_DEFAULT)
		sendCtrl(SOF_REPLAY_VOLUME, kSofVolGain[mVolLevel], "capture volume");
	if (mMuted)
		sendCtrl(SOF_REPLAY_SWITCH, 0, "capture switch");

	/* trigger START: host DMA first, then IPC (sof_pcm_trigger, ipc_first=false) */
	mLdp0 = hda32(mPpCap + 0x10 + 0x10 * (UInt32)s->index + 0x8);
	hdaUpdate32(s->sd, SOF_SD_CTL_DMA_START | SOF_SD_INT_MASK, SOF_SD_CTL_DMA_START | SOF_SD_INT_MASK);
	if (!replayOne(21)) {
		dmicHwStop();
		replayOne(23);
		mPcmParams = false;
		return false;
	}
	mCapturing = true;
	mTicks = 0;
	mLastLdp = 0xFFFFFFFFU;
	SOFLOG("DMIC capture running: ctl %08x ldp0 %08x", (unsigned)hda32(s->sd), (unsigned)mLdp0);
	return true;
}

/* trigger STOP: IPC first, then host DMA; then PCM_FREE. Buffers are kept. Caller holds mLock. */
void VoodooHDASof::dmicStop()
{
	if (mCapturing) {
		replayOne(22);
		mCapturing = false;
	}
	if (mDmic.buf)
		dmicHwStop();
	if (mPcmParams) {
		replayOne(23);
		mPcmParams = false;
	}
}

/* ---- public capture API (VoodooHDASofEngine) ---- */

bool VoodooHDASof::captureReady()
{
	return mFwReady && mTopology && mDmic.buf;
}

bool VoodooHDASof::captureStart()
{
	IOLockLock(mLock);
	bool ok = mDsp && dmicStart();
	IOLockUnlock(mLock);
	return ok;
}

void VoodooHDASof::captureStop()
{
	IOLockLock(mLock);
	if (mDsp && mFwReady)
		dmicStop();
	IOLockUnlock(mLock);
}

/* current write position in the ring, in frames (host DMA linear position since start) */
UInt32 VoodooHDASof::captureFrame()
{
	if (!mDsp || !mCapturing || mDmic.index < 0)
		return 0;
	UInt32 ldp = hda32(mPpCap + 0x10 + 0x10 * (UInt32)mDmic.index + 0x8);
	return ((ldp - mLdp0) % SOF_DMIC_BUF_SIZE) / SOF_DMIC_FRAME_BYTES;
}

void *VoodooHDASof::captureBuffer()
{
	return mDmic.buf ? (void *)mDmic.buf->virtAddr : NULL;
}

bool VoodooHDASof::setCaptureVolume(UInt32 level)
{
	if (level > SOF_VOL_STEPS)
		level = SOF_VOL_STEPS;
	IOLockLock(mLock);
	bool ok = sendCtrl(SOF_REPLAY_VOLUME, kSofVolGain[level], "capture volume");
	if (ok)
		mVolLevel = level;
	IOLockUnlock(mLock);
	return ok;
}

bool VoodooHDASof::setCaptureMute(bool mute)
{
	IOLockLock(mLock);
	bool ok = sendCtrl(SOF_REPLAY_SWITCH, mute ? 0 : 1, "capture switch");
	if (ok)
		mMuted = mute;
	IOLockUnlock(mLock);
	return ok;
}

/* integer 20*log10(x/32768) in tenths of dB (x > 0), ~0.3 dB accurate */
static int dbfsTenths(UInt32 x)
{
	if (!x)
		return -999;
	int msb = 31 - __builtin_clz(x);
	UInt32 frac = (msb >= 8) ? ((x >> (msb - 8)) & 0xFF) : ((x << (8 - msb)) & 0xFF);
	/* log2(x) ~ msb + frac/256 (linear mantissa, max error ~0.086) */
	int l2x256 = msb * 256 + (int)frac;
	return ((l2x256 - 15 * 256) * 602) / 2560;	/* 6.02 dB per bit, tenths */
}

/* once per second: RMS/peak per channel over the whole 64 KB ring + position + notifications */
void VoodooHDASof::logCapture()
{
	SofStream *s = &mDmic;
	UInt32 pphc = mPpCap + 0x10 + 0x10 * (UInt32)s->index;
	UInt32 ldp = hda32(pphc + 0x8), sts = hda8(s->sd + SOF_SD_STS);
	const SInt16 *smp = (const SInt16 *)s->buf->virtAddr;
	UInt64 sq[2] = { 0, 0 };
	UInt32 peak[2] = { 0, 0 }, n = SOF_DMIC_BUF_SIZE / 4, nonzero = 0;

	serviceNotification();
	for (UInt32 i = 0; i < n; i++) {
		for (int c = 0; c < 2; c++) {
			SInt32 x = smp[2 * i + (UInt32)c];
			UInt32 a = (UInt32)(x < 0 ? -x : x);
			sq[c] += (UInt64)a * a;
			if (a > peak[c])
				peak[c] = a;
			if (x)
				nonzero++;
		}
	}
	UInt32 rms[2];
	for (int c = 0; c < 2; c++) {
		UInt64 m = sq[c] / n, r = 0, bit = 1ULL << 62;
		while (bit > m) bit >>= 2;
		while (bit) { if (m >= r + bit) { m -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
		rms[c] = (UInt32)r;
	}
	int dl = dbfsTenths(rms[0]), dr = dbfsTenths(rms[1]);
	SOFLOG("rec t=%3us ldp %05x sts %02x | L rms %5u (%d.%d dBFS) peak %5u | R rms %5u (%d.%d dBFS) peak %5u | nz %u",
	       (unsigned)mTicks, (unsigned)ldp, (unsigned)sts,
	       (unsigned)rms[0], dl / 10, (dl < 0 ? -dl : dl) % 10, (unsigned)peak[0],
	       (unsigned)rms[1], dr / 10, (dr < 0 ? -dr : dr) % 10, (unsigned)peak[1], (unsigned)nonzero);

	/* if the host DMA never moves, try once with the firmware's non-snoop setting */
	if (mTicks == 3 && ldp == mLastLdp && !nonzero && !mNsnpFallback && mDev->mOrigPciCfgValid) {
		UInt16 fw = (UInt16)mDev->mOrigPciCfg[SOF_PCI_DEVC / 4] & SOF_PCI_DEVC_NSNPEN;
		UInt16 cur = mPci->configRead16(SOF_PCI_DEVC);
		if (fw && !(cur & SOF_PCI_DEVC_NSNPEN)) {
			mPci->configWrite16(SOF_PCI_DEVC, (UInt16)(cur | SOF_PCI_DEVC_NSNPEN));
			mNsnpFallback = true;
			SOFLOG("no data after 3 s: DEVC NSNPEN -> firmware value (%04x)", (unsigned)mPci->configRead16(SOF_PCI_DEVC));
		}
	}
	mLastLdp = ldp;
}

void VoodooHDASof::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
	VoodooHDASof *self = OSDynamicCast(VoodooHDASof, owner);

	if (!self)
		return;
	IOLockLock(self->mLock);
	if (!self->mCapturing) {
		IOLockUnlock(self->mLock);
		return;
	}
	self->mTicks++;
	self->logCapture();
	if (self->mTicks >= SOF_DMIC_LOG_SECONDS) {
		SOFLOG("capture test finished after %u s, stopping", (unsigned)self->mTicks);
		self->dmicStop();
		self->teardownTopology();
		IOLockUnlock(self->mLock);
		if (self->mNsnpFallback) {
			self->restoreDevc();
			self->mPci->configWrite16(SOF_PCI_DEVC, self->mSavedDevc);
			self->mNsnpFallback = false;
		}
		return;
	}
	IOLockUnlock(self->mLock);
	sender->setTimeoutMS(1000);
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
	dumpConfig();
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
	if (!ok || mStep < kSofStepIpc || !mFwReady)
		return ok;

	/* M3a: mailbox (parsed at FW_READY) + topology */
	if (!mHostBoxSize || !mDspBoxSize) {
		SOFLOG("no mailbox windows, IPC not possible");
		return true;		/* DSP stays booted; no IPC */
	}
	SOFLOG("replaying topology (#0-19)");
	mTopology = true;		/* partial topology is freed as well */
	if (!replay(0, 19)) {
		teardownTopology();
		return true;
	}
	if (mStep < kSofStepCapture) {
		SOFLOG("step %u reached (IPC OK)", (unsigned)mStep);
		teardownTopology();
		return true;
	}

	if (!dmicAlloc()) {
		teardownTopology();
		return true;
	}
	if (mStep >= kSofStepEngine) {
		SOFLOG("ready for the capture engine (topology loaded, buffers allocated)");
		return true;
	}

	/* M3b test: DMIC capture + 1 s RMS log on our own workloop */
	if (!dmicStart()) {
		dmicStop();
		teardownTopology();
		return true;
	}
	mWorkLoop = IOWorkLoop::workLoop();
	mTimer = mWorkLoop ? IOTimerEventSource::timerEventSource(this, &VoodooHDASof::timerFired) : NULL;
	if (!mTimer || mWorkLoop->addEventSource(mTimer) != kIOReturnSuccess) {
		SOFLOG("error: no timer, stopping capture");
		dmicStop();
		teardownTopology();
		return true;
	}
	mTimer->setTimeoutMS(1000);
	return true;
}

void VoodooHDASof::stop()
{
	SOFDBG("stop");
	if (mTimer) {
		mTimer->cancelTimeout();
		mTimer->disable();
		if (mWorkLoop)
			mWorkLoop->removeEventSource(mTimer);
		mTimer->release();
		mTimer = NULL;
	}
	if (mWorkLoop) {
		mWorkLoop->release();
		mWorkLoop = NULL;
	}
	if (mLock)
		IOLockLock(mLock);
	powerDownDsp();
	if (mLock)
		IOLockUnlock(mLock);
	if (mDspMap) {
		mDspMap->release();
		mDspMap = NULL;
		mDsp = NULL;
	}
}

void VoodooHDASof::free()
{
	stop();
	dmicFree();		/* only now: the capture engine may have used the buffer until here */
	if (mLock) {
		IOLockFree(mLock);
		mLock = NULL;
	}
	OSObject::free();
}
