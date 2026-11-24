// SPDX-License-Identifier: BSD-3-Clause
/*
 * VoodooHDASof — Intel SOF (cAVS 2.5 / ADL) audio DSP support for VoodooHDA,
 * used for the DSP-attached digital microphones (DMIC).
 *
 * Register layout follows the Linux SOF Intel driver (sound/soc/sof/intel/hda.h,
 * include/sound/hda_register.h), which is dual licensed "GPL-2.0-only OR BSD-3-Clause"
 * for the SOF parts. Copyright(c) 2017-2022 Intel Corporation.
 *
 * Everything here is gated by the boot-arg -voodoosof; without it VoodooHDA behaves
 * exactly like stock.
 *   -voodoosof          enable
 *   -voodoosofdbg       verbose logging
 *   voodoosofstep=N     stop after milestone step N (default: highest implemented)
 */

#ifndef _VOODOO_HDA_SOF_H
#define _VOODOO_HDA_SOF_H

#include <libkern/c++/OSObject.h>
#include <IOKit/IOMemoryDescriptor.h>

class VoodooHDADevice;
class IOPCIDevice;

/* ---- PCI config space (hda.h) ---- */
#define SOF_PCI_PGCTL			0x44	/* == TCSEL on legacy HDA */
#define SOF_PCI_PGCTL_ADSPPGD		(1U << 2)
#define SOF_PCI_CGCTL			0x48
#define SOF_PCI_CGCTL_ADSPDCGE		(1U << 1)

/* ---- HDA BAR0: capability list (hda_register.h) ---- */
#define SOF_HDA_LLCH			0x14
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
#define SOF_ML_MLCD			0x04
#define SOF_ML_BASE			0x40
#define SOF_ML_INTERVAL			0x40
#define SOF_ML_LCAP			0x00
#define SOF_ML_LCTL			0x04

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
#define SOF_DSP_MBOX_OFFSET		0x80000	/* SRAM window 0 */
#define SOF_SRAM_ROM_STATUS		(SOF_DSP_MBOX_OFFSET + 0x0)
#define SOF_SRAM_ROM_ERROR		(SOF_DSP_MBOX_OFFSET + 0x4)
#define SOF_ADSPCS_CRST(cm)		((cm) << 0)
#define SOF_ADSPCS_CSTALL(cm)		((cm) << 8)
#define SOF_ADSPCS_SPA(cm)		((cm) << 16)
#define SOF_ADSPCS_CPA(cm)		((cm) << 24)

/* Milestones (voodoosofstep) */
enum {
	kSofStepProbe = 1,	/* M1: map BAR4, read-only register dump */
	kSofStepMax = kSofStepProbe
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

	bool initWithDevice(VoodooHDADevice *device);
	bool mapDsp();
	void findCaps();
	void dumpState(const char *when);
	void dumpStreams();

	UInt32 hda32(UInt32 off);
	UInt32 dsp32(UInt32 off);
};

#endif /* _VOODOO_HDA_SOF_H */
