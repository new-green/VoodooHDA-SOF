// SPDX-License-Identifier: BSD-3-Clause
/*
 * VoodooHDASofEngine — IOAudioEngine for the Intel SOF DSP digital microphones
 * ("Dahili Mikrofon"). Capture only, 48 kHz / 2 ch / S16LE. The sample buffer is the
 * DMIC host DMA ring owned by VoodooHDASof; the position comes from the host DMA linear
 * position (no interrupts), and the buffer-wrap time stamps are taken from a poll timer.
 */

#ifndef _VOODOO_HDA_SOF_ENGINE_H
#define _VOODOO_HDA_SOF_ENGINE_H

#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/IOTimerEventSource.h>

class VoodooHDASof;
class IOAudioStream;

#define SOF_ENGINE_POLL_MS		5
#define SOF_ENGINE_INPUT_OFFSET		512	/* frames the HAL stays behind the DMA write position */

class VoodooHDASofEngine : public IOAudioEngine
{
	OSDeclareDefaultStructors(VoodooHDASofEngine)

public:
	bool initWithSof(VoodooHDASof *sof);
	virtual void free() override;
	virtual bool initHardware(IOService *provider) override;
	virtual void stop(IOService *provider) override;

	virtual IOReturn performAudioEngineStart() override;
	virtual IOReturn performAudioEngineStop() override;
	virtual UInt32 getCurrentSampleFrame() override;
	virtual IOReturn performFormatChange(IOAudioStream *audioStream, const IOAudioStreamFormat *newFormat,
			const IOAudioSampleRate *newSampleRate) override;
	virtual IOReturn convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame,
			UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream) override;

private:
	VoodooHDASof *mSof;		/* retained */
	IOTimerEventSource *mPosTimer;
	UInt32 mLastFrame;
	bool mRunning;
	UInt32 mWraps;

	void pollPosition();
	static void posTimerFired(OSObject *owner, IOTimerEventSource *sender);
};

#endif /* _VOODOO_HDA_SOF_ENGINE_H */
