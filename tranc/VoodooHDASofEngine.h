// SPDX-License-Identifier: BSD-3-Clause
/*
 * VoodooHDASofEngine — IOAudioEngine for the Intel SOF DSP digital microphones
 * ("Internal Microphone"). Capture only, 48 kHz / 2 ch / S16LE. The sample buffer is the
 * DMIC host DMA ring owned by VoodooHDASof; the position comes from the host DMA linear
 * position (no interrupts), and the buffer-wrap time stamps are taken from a poll timer.
 */

#ifndef _VOODOO_HDA_SOF_ENGINE_H
#define _VOODOO_HDA_SOF_ENGINE_H

#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/IOTimerEventSource.h>

class VoodooHDASof;
class IOAudioStream;
class IOAudioControl;

#define SOF_ENGINE_POLL_MS		5
#define SOF_ENGINE_INPUT_OFFSET		512	/* frames the HAL stays behind the DMA write position */

/* noise reduction of the second engine (RNNoise, mono) */
enum { kSofNrOff = 0, kSofNrLight = 1, kSofNrStrong = 2 };
#define SOF_NR_STRENGTH_MAX		100	/* user strength 0..100 %: band gain floor = -0.4 dB per % (100 = no floor) */
#define SOF_NR_STRENGTH_LIGHT		50	/* -20 dB, default */
#define SOF_NR_STRENGTH_KEY		"SofNrStrength"	/* registry property, set from user space (SofMicBar) */
#define SOF_RING_FRAMES			(SOF_DMIC_BUF_SIZE / SOF_DMIC_FRAME_BYTES)
#define SOF_NR_MAX_FEED			4096	/* a bigger forward jump skips ahead instead of denoising the gap */
#define SOF_NR_MAX_AGE			(SOF_RING_FRAMES - 2048)	/* older input may be overwritten by DMA */
#define SOF_NR_PREROLL			(2 * RNNK_FRAME)	/* fed before a (re)start point: no silent gap */
#define SOF_NR_LIMIT			29204.0f	/* -1 dBFS in 16-bit scale */
#define SOF_NR_LIMIT_RELEASE		0.000208312f	/* 100 ms at 48 kHz */

class VoodooHDASofEngine : public IOAudioEngine
{
	OSDeclareDefaultStructors(VoodooHDASofEngine)

public:
	bool initWithSof(VoodooHDASof *sof, UInt32 nrMode);
	/* the other engine on the same DMIC stream: input level and mute are one DSP setting */
	void setPeer(VoodooHDASofEngine *peer) { mPeer = peer; }
	virtual void free() override;
	virtual bool initHardware(IOService *provider) override;
	virtual void stop(IOService *provider) override;
	virtual IOReturn setProperties(OSObject *properties) override;

	virtual IOReturn performAudioEngineStart() override;
	virtual IOReturn performAudioEngineStop() override;
	virtual UInt32 getCurrentSampleFrame() override;
	virtual IOReturn performFormatChange(IOAudioStream *audioStream, const IOAudioStreamFormat *newFormat,
			const IOAudioSampleRate *newSampleRate) override;
	virtual IOReturn convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame,
			UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream) override;

private:
	VoodooHDASof *mSof;		/* retained */
	VoodooHDASofEngine *mPeer;	/* not retained; cleared by stop() */
	IOAudioControl *mVolCtl;	/* owned by the engine's control list */
	IOAudioControl *mMuteCtl;
	IOTimerEventSource *mPosTimer;
	UInt32 mLastFrame;
	bool mRunning;
	UInt32 mWraps;

	/* noise reduction (convertInputSamples, serialized by mNrLock) */
	IOLock *mNrLock;
	UInt32 mNrMode;
	bool mNrSync;			/* false: restart at the next read */
	UInt32 mNrInPos;		/* ring frame of the next input frame to feed */
	UInt32 mNrBlockStart;		/* ring frame of mNrIn[0] */
	UInt32 mNrFill;
	float *mNrIn;			/* RNNK_FRAME mono input frames */
	float *mNrBlk;			/* RNNK_FRAME output frames */
	float *mNrOut;			/* SOF_RING_FRAMES mono output, ring positions */
	UInt32 mNrBlocks;
	UInt32 mNrSkips;		/* forward jumps / overwritten input: skipped ahead */
	UInt32 mNrBehind;		/* reads entirely inside already denoised frames */
	UInt32 mNrMaxGap;		/* largest gap denoised in one read */
	double mHpX1, mHpX2, mHpY1, mHpY2;	/* 90 Hz high-pass before RNNoise */
	float mLimGain;				/* peak limiter after RNNoise */
	UInt32 mLimHits;
	volatile UInt32 mNrStrength;		/* requested by setProperties */
	UInt32 mNrStrengthApplied;		/* in RNNoise (set under mNrLock) */
	void nrReset();

	void pollPosition();
	bool createControls();
	void nrFeed(const SInt16 *ring, UInt32 endFrame);
	static IOReturn volumeChanged(OSObject *target, IOAudioControl *control, SInt32 oldValue, SInt32 newValue);
	static IOReturn muteChanged(OSObject *target, IOAudioControl *control, SInt32 oldValue, SInt32 newValue);
	static void posTimerFired(OSObject *owner, IOTimerEventSource *sender);
};

#endif /* _VOODOO_HDA_SOF_ENGINE_H */
