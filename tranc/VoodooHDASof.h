// SPDX-License-Identifier: BSD-3-Clause
/*
 * VoodooHDASof — Intel SOF (cAVS 2.5 / ADL) audio DSP support for VoodooHDA,
 * used for the DSP-attached digital microphones (DMIC).
 *
 * Register layout and boot sequence follow the Linux SOF Intel driver
 * (sound/soc/sof/intel/{hda.h,hda-dsp.c,hda-loader.c,hda-stream.c,hda-ctrl.c,cnl.c},
 * include/sound/hda_register.h, include/sound/sof/{header,info,stream}.h), whose SOF parts are dual licensed
 * "GPL-2.0-only OR BSD-3-Clause". Copyright(c) 2017-2022 Intel Corporation.
 *
 * Everything here is gated by the boot-arg -voodoosof; without it VoodooHDA behaves
 * exactly like stock.
 *   -voodoosof          enable
 *   -voodoosofdbg       verbose logging
 *   voodoosofstep=N     stop after milestone step N (default: highest implemented)
 *                       1 = read-only probe, 2 = DSP power-up + ROM init (then power down again),
 *                       3 = full firmware load until FW_READY
 */

#ifndef _VOODOO_HDA_SOF_H
#define _VOODOO_HDA_SOF_H

#include <libkern/c++/OSObject.h>
#include <IOKit/IOMemoryDescriptor.h>

class VoodooHDADevice;
class IOPCIDevice;
struct _DmaMemory;

/* ---- PCI config space (hda.h) ---- */
#define SOF_PCI_PGCTL			0x44	/* == TCSEL on legacy HDA */
#define SOF_PCI_PGCTL_ADSPPGD		(1U << 2)
#define SOF_PCI_CGCTL			0x48
#define SOF_PCI_CGCTL_ADSPDCGE		(1U << 1)

/* ---- HDA BAR0 (hda_register.h, hda.h) ---- */
#define SOF_HDA_LLCH			0x14
#define SOF_HDA_INTCTL			0x20
#define SOF_CAP_HDR_VER(x)		(((x) >> 28) & 0xF)
#define SOF_CAP_HDR_ID(x)		(((x) >> 16) & 0xFFF)
#define SOF_CAP_HDR_NXT(x)		((x) & 0xFFFF)
#define SOF_GTS_CAP_ID			0x1
#define SOF_ML_CAP_ID			0x2
#define SOF_PP_CAP_ID			0x3
#define SOF_SPB_CAP_ID			0x4
#define SOF_DRSM_CAP_ID			0x5
#define SOF_MAX_CAPS			10
#define SOF_PP_PPCTL			0x04
#define SOF_PP_PPSTS			0x08
#define SOF_PPCTL_PIE			(1U << 31)
#define SOF_PPCTL_GPROCEN		(1U << 30)
#define SOF_ML_MLCD			0x04
#define SOF_ML_BASE			0x40
#define SOF_ML_INTERVAL			0x40
#define SOF_ML_LCAP			0x00
#define SOF_ML_LCTL			0x04
#define SOF_SPB_SPBFCCTL		0x04
#define SOF_SPB_SPIB(i)			((UInt32)(0x08 + 0x08 * (i)))
#define SOF_VS_EM2			0x1030
#define SOF_VS_EM2_L1SEN		(1U << 13)
#define SOF_VS_D0I3C			0x104A
#define SOF_VS_LTRP			0x1048
#define SOF_VS_LTRP_GB_MASK		0x3F
#define SOF_LTRP_GB_VALUE_US		95

/* stream descriptors */
#define SOF_SD_BASE			0x80
#define SOF_SD_SIZE			0x20
#define SOF_SD_OFFSET(i)		((UInt32)(SOF_SD_BASE + SOF_SD_SIZE * (i)))
#define SOF_SD_CTL			0x00
#define SOF_SD_STS			0x03
#define SOF_SD_LPIB			0x04
#define SOF_SD_CBL			0x08
#define SOF_SD_LVI			0x0C
#define SOF_SD_FIFOSIZE			0x10
#define SOF_SD_FORMAT			0x12
#define SOF_SD_BDLPL			0x18
#define SOF_SD_BDLPU			0x1C
#define SOF_SD_CTL_SRST			0x01
#define SOF_SD_CTL_DMA_START		0x02
#define SOF_SD_INT_MASK			0x1C	/* DESC_ERR | FIFO_ERR | COMPLETE */
#define SOF_SD_CTL_TAG_SHIFT		20
#define SOF_SD_CTL_TAG_MASK		(0xFU << SOF_SD_CTL_TAG_SHIFT)
#define SOF_CL_STREAM_FORMAT		0x40
#define SOF_BDL_SIZE			4096

/* ---- DSP BAR4 (hda.h) ---- */
#define SOF_DSP_BAR_REG			0x20	/* kIOPCIConfigBaseAddress4 */
#define SOF_DSP_REG_ADSPCS		0x04
#define SOF_DSP_REG_ADSPIC		0x08
#define SOF_DSP_REG_ADSPIS		0x0C
#define SOF_DSP_REG_ADSPIC2		0x10
#define SOF_DSP_REG_ADSPIS2		0x14
#define SOF_CNL_HIPCTDR			0xC0
#define SOF_CNL_HIPCTDA			0xC4
#define SOF_CNL_HIPCTDD			0xC8
#define SOF_CNL_HIPCIDR			0xD0
#define SOF_CNL_HIPCIDA			0xD4
#define SOF_CNL_HIPCIDD			0xD8
#define SOF_CNL_HIPCCTL			0xE8
#define SOF_IPC_BUSY			(1U << 31)	/* HIPCIDR / HIPCTDR */
#define SOF_IPC_DONE			(1U << 31)	/* HIPCIDA / HIPCTDA */
#define SOF_IPC_MSG_MASK		0x7FFFFFFFU
#define SOF_DSP_MBOX_OFFSET		0x80000	/* SRAM window 0 */
#define SOF_DSP_MBOX_UPLINK_OFFSET	0x81000
#define SOF_SRAM_ROM_STATUS		(SOF_DSP_MBOX_OFFSET + 0x0)
#define SOF_SRAM_ROM_ERROR		(SOF_DSP_MBOX_OFFSET + 0x4)
#define SOF_ADSPCS_CRST(cm)		((cm) << 0)
#define SOF_ADSPCS_CSTALL(cm)		((cm) << 8)
#define SOF_ADSPCS_SPA(cm)		((cm) << 16)
#define SOF_ADSPCS_CPA(cm)		((cm) << 24)
#define SOF_ROM_IPC_CONTROL		0x01000000U
#define SOF_ROM_IPC_PURGE_FW		0x00004000U
#define SOF_FSR_STATE(x)		((x) & 0xFFFFFFU)
#define SOF_FSR_INIT_DONE		0x1
#define SOF_FSR_FW_ENTERED		0x5
#define SOF_SSP_BASE			0x10000
#define SOF_SSP_SIZE			0x1000
#define SOF_SSP_COUNT			3	/* TGL_SSP_COUNT */
#define SOF_SSP_SSC1			0x4
#define SOF_SSP_CBP_CFP			((1U << 25) | (1U << 24))

/* IPC3 */
#define SOF_IPC_FW_READY		0x70000000U
#define SOF_IPC_PANIC_MAGIC		0x0dead000U
#define SOF_IPC_PANIC_MAGIC_MASK	0x0ffff000U
#define SOF_EXT_MAN_MAGIC		0x6e614d58U	/* "XMan" */

/* timeouts (hda.h) */
#define SOF_RESET_TIMEOUT_US		50000
#define SOF_PD_TIMEOUT_US		50000
#define SOF_INIT_TIMEOUT_US		500000
#define SOF_ROM_INIT_TIMEOUT_US		300000
#define SOF_BASEFW_TIMEOUT_US		3000000
#define SOF_BOOT_TIMEOUT_US		2000000
#define SOF_STREAM_RUN_TIMEOUT_US	300
#define SOF_FW_BOOT_ATTEMPTS		3

#define SOF_CORE0			1U	/* init_core_mask == host_managed_cores_mask */

/* Milestones (voodoosofstep) */
enum {
	kSofStepProbe = 1,	/* M1: map BAR4, read-only register dump */
	kSofStepRom = 2,	/* M2a: core 0 power-up + ROM init, then power down again */
	kSofStepFw = 3,		/* M2b: code loader DMA, FW_ENTERED, FW_READY */
	kSofStepMax = kSofStepFw
};

struct SofStream {
	int index;		/* global SD index */
	int tag;		/* stream tag = per-direction index + 1 (DSP uses tag-1 as DMA id) */
	bool playback;
	UInt32 sd;		/* SD register offset in BAR0 */
	UInt32 size;
	int frags;
	struct _DmaMemory *buf;
	struct _DmaMemory *bdl;
};

class VoodooHDASof : public OSObject
{
	OSDeclareDefaultStructors(VoodooHDASof)

public:
	/* true when -voodoosof is present and the controller is a supported DSP part */
	static bool isEnabled(UInt32 deviceId);
	static VoodooHDASof *withDevice(VoodooHDADevice *device);

	bool start();
	void stop();
	virtual void free() override;

private:
	VoodooHDADevice *mDev;		/* owner, not retained */
	IOPCIDevice *mPci;
	IOMemoryMap *mDspMap;
	volatile UInt8 *mDsp;
	IOByteCount mDspLen;
	IOByteCount mHdaLen;

	UInt32 mPpCap, mMlCap, mSpbCap, mDrsmCap, mGtsCap;	/* BAR0 offsets, 0 = absent */
	UInt32 mStep;
	bool mDbg;

	bool mDspPowered;		/* core 0 powered by us */
	bool mFwReady;
	bool mGatingDisabled;
	bool mSavedEm2L1;
	UInt8 mSavedLtrp;
	bool mLow4G;			/* experiment: DMA buffers below 4 GB */
	bool mLinuxIntBits;		/* experiment: SD int enables + BDL IOC exactly like Linux */
	SofStream mLoader;		/* code loader (playback) */
	SofStream mIccmax;		/* ICCMAX workaround (capture) */

	bool initWithDevice(VoodooHDADevice *device);
	bool mapDsp();
	void findCaps();
	void dumpConfig();
	void dumpState(const char *when);
	void dumpStreams();

	/* register access */
	UInt32 hda32(UInt32 off);
	UInt16 hda16(UInt32 off);
	UInt8 hda8(UInt32 off);
	void hdaW32(UInt32 off, UInt32 val);
	void hdaW16(UInt32 off, UInt16 val);
	void hdaW8(UInt32 off, UInt8 val);
	void hdaUpdate32(UInt32 off, UInt32 mask, UInt32 val);
	UInt32 dsp32(UInt32 off);
	void dspW32(UInt32 off, UInt32 val);
	void dspUpdate32(UInt32 off, UInt32 mask, UInt32 val);
	void pciUpdate32(UInt32 off, UInt32 mask, UInt32 val);
	bool pollHda(UInt32 off, UInt32 mask, UInt32 want, UInt32 timeoutUs, UInt32 *last = NULL);
	bool pollDsp(UInt32 off, UInt32 mask, UInt32 want, UInt32 timeoutUs, UInt32 *last = NULL);

	/* controller / DSP core (hda-ctrl.c, hda-dsp.c) */
	void clockPowerGating(bool enable);
	bool corePowerUp(UInt32 cm);
	bool coreRun(UInt32 cm);
	bool coreIsEnabled(UInt32 cm);
	bool coreStallReset(UInt32 cm);
	bool coreResetPowerDown(UInt32 cm);
	void sspSetConsumer();

	/* streams (hda-stream.c, hda-loader.c) */
	bool pickStreams(bool linuxStreams);
	struct _DmaMemory *allocDma(UInt32 size, const char *what);
	bool streamAlloc(SofStream *s, UInt32 size, const char *what);
	void streamFree(SofStream *s);
	int streamSetupBdl(SofStream *s);
	bool streamReset(SofStream *s);
	bool loaderPrepare(const UInt8 *fw, UInt32 fwSize);
	bool loaderTrigger(bool start);
	void loaderCleanup();
	bool iccmaxStart();
	void iccmaxCleanup();

	/* boot (hda-loader.c, cnl.c, ipc3.c) */
	bool clDspInit(int tag);
	bool bootFirmware();
	bool traceCopy(UInt32 *lastFsr);
	bool waitFwReady();
	void abortBoot(const char *why);
	void powerDownDsp();
};

#endif /* _VOODOO_HDA_SOF_H */
