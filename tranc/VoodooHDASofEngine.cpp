// SPDX-License-Identifier: BSD-3-Clause
/*
 * VoodooHDASofEngine — see VoodooHDASofEngine.h.
 */

#include "VoodooHDASofEngine.h"
#include "VoodooHDASof.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioTypes.h>
#include <IOKit/audio/IOAudioDefines.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>

#define ENGLOG(fmt, ...)	IOLog("VoodooHDA SOF engine: " fmt "\n", ##__VA_ARGS__)

#define super IOAudioEngine
OSDefineMetaClassAndStructors(VoodooHDASofEngine, IOAudioEngine)

bool VoodooHDASofEngine::initWithSof(VoodooHDASof *sof)
{
	if (!super::init(NULL) || !sof || !sof->captureBuffer())
		return false;
	mSof = sof;
	mSof->retain();
	return true;
}

void VoodooHDASofEngine::free()
{
	if (mPosTimer) {
		mPosTimer->release();
		mPosTimer = NULL;
	}
	if (mSof) {
		mSof->release();
		mSof = NULL;
	}
	super::free();
}

bool VoodooHDASofEngine::initHardware(IOService *provider)
{
	IOAudioSampleRate rate = { SOF_DMIC_RATE, 0 };
	IOAudioStreamFormat format = {
		SOF_DMIC_CHANNELS,				/* channels */
		kIOAudioStreamSampleFormatLinearPCM,
		kIOAudioStreamNumericRepresentationSignedInt,
		16,						/* bit depth */
		16,						/* bit width */
		kIOAudioStreamAlignmentLowByte,
		kIOAudioStreamByteOrderLittleEndian,
		true,						/* mixable */
		0
	};
	IOAudioStreamFormatExtension formatEx = {
		kFormatExtensionCurrentVersion, 0, 1, SOF_DMIC_FRAME_BYTES
	};
	IOAudioStream *stream;

	if (!super::initHardware(provider))
		return false;

	setDescription("Dahili Mikrofon");
	setSampleRate(&rate);
	setNumSampleFramesPerBuffer(SOF_DMIC_BUF_SIZE / SOF_DMIC_FRAME_BYTES);
	setInputSampleOffset(SOF_ENGINE_INPUT_OFFSET);
	setSampleLatency(SOF_ENGINE_INPUT_OFFSET);
	setClockIsStable(true);

	stream = new IOAudioStream;
	if (!stream || !stream->initWithAudioEngine(this, kIOAudioStreamDirectionInput, 1)) {
		ENGLOG("error: IOAudioStream init failed");
		if (stream)
			stream->release();
		return false;
	}
	stream->setSampleBuffer(mSof->captureBuffer(), SOF_DMIC_BUF_SIZE);
	stream->addAvailableFormat(&format, &formatEx, &rate, &rate);
	addAudioStream(stream);
	stream->setFormat(&format, &formatEx);
	stream->setTerminalType(INPUT_MICROPHONE);
	stream->release();

	if (!createControls())
		ENGLOG("warning: no input level controls");

	mPosTimer = IOTimerEventSource::timerEventSource(this, &VoodooHDASofEngine::posTimerFired);
	if (!mPosTimer || !getWorkLoop() || getWorkLoop()->addEventSource(mPosTimer) != kIOReturnSuccess) {
		ENGLOG("error: no position timer");
		return false;
	}
	ENGLOG("ready: 48 kHz, 2 ch, S16LE, %u frames", (unsigned)(SOF_DMIC_BUF_SIZE / SOF_DMIC_FRAME_BYTES));
	return true;
}

/* "Input level" slider (-50..+20 dB, 1 dB steps, bottom = mute) and input mute, both channels together */
bool VoodooHDASofEngine::createControls()
{
	IOAudioControl *vol, *mute;

	vol = IOAudioLevelControl::createVolumeControl(SOF_VOL_DEFAULT, 0, SOF_VOL_STEPS,
			(IOFixed)(SOF_VOL_MIN_DB * 65536), (IOFixed)(SOF_VOL_MAX_DB * 65536),
			kIOAudioControlChannelIDAll, kIOAudioControlChannelNameAll, 0, kIOAudioControlUsageInput);
	if (!vol)
		return false;
	vol->setValueChangeHandler(&VoodooHDASofEngine::volumeChanged, this);
	addDefaultAudioControl(vol);
	vol->release();

	mute = IOAudioToggleControl::createMuteControl(false, kIOAudioControlChannelIDAll,
			kIOAudioControlChannelNameAll, 0, kIOAudioControlUsageInput);
	if (!mute)
		return false;
	mute->setValueChangeHandler(&VoodooHDASofEngine::muteChanged, this);
	addDefaultAudioControl(mute);
	mute->release();
	return true;
}

IOReturn VoodooHDASofEngine::volumeChanged(OSObject *target, IOAudioControl *, SInt32 oldValue, SInt32 newValue)
{
	VoodooHDASofEngine *self = OSDynamicCast(VoodooHDASofEngine, target);

	if (!self || newValue < 0)
		return kIOReturnBadArgument;
	ENGLOG("input level %d -> %d (%d dB)", (int)oldValue, (int)newValue, (int)newValue + SOF_VOL_MIN_DB);
	return self->mSof->setCaptureVolume((UInt32)newValue) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn VoodooHDASofEngine::muteChanged(OSObject *target, IOAudioControl *, SInt32 oldValue, SInt32 newValue)
{
	VoodooHDASofEngine *self = OSDynamicCast(VoodooHDASofEngine, target);

	if (!self)
		return kIOReturnBadArgument;
	ENGLOG("input mute %s", newValue ? "on" : "off");
	return self->mSof->setCaptureMute(newValue != 0) ? kIOReturnSuccess : kIOReturnError;
}

void VoodooHDASofEngine::stop(IOService *provider)
{
	if (mPosTimer) {
		mPosTimer->cancelTimeout();
		if (getWorkLoop())
			getWorkLoop()->removeEventSource(mPosTimer);
	}
	if (mRunning) {
		mRunning = false;
		mSof->captureStop();
	}
	super::stop(provider);
}

IOReturn VoodooHDASofEngine::performAudioEngineStart()
{
	if (!mSof->captureStart()) {
		ENGLOG("error: capture start failed");
		return kIOReturnError;
	}
	mRunning = true;
	mLastFrame = 0;
	mWraps = 0;
	takeTimeStamp(false);
	if (mPosTimer)
		mPosTimer->setTimeoutMS(SOF_ENGINE_POLL_MS);
	ENGLOG("started");
	return kIOReturnSuccess;
}

IOReturn VoodooHDASofEngine::performAudioEngineStop()
{
	if (mPosTimer)
		mPosTimer->cancelTimeout();
	if (mRunning) {
		mRunning = false;
		mSof->captureStop();
	}
	ENGLOG("stopped after %u buffer wraps", (unsigned)mWraps);
	return kIOReturnSuccess;
}

UInt32 VoodooHDASofEngine::getCurrentSampleFrame()
{
	return mSof->captureFrame();
}

IOReturn VoodooHDASofEngine::performFormatChange(IOAudioStream *, const IOAudioStreamFormat *newFormat,
		const IOAudioSampleRate *newSampleRate)
{
	/* single fixed format */
	if (newSampleRate && newSampleRate->whole != SOF_DMIC_RATE)
		return kIOReturnUnsupported;
	if (newFormat && (newFormat->fNumChannels != SOF_DMIC_CHANNELS || newFormat->fBitWidth != 16))
		return kIOReturnUnsupported;
	return kIOReturnSuccess;
}

IOReturn VoodooHDASofEngine::convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame,
		UInt32 numSampleFrames, const IOAudioStreamFormat *, IOAudioStream *)
{
	const SInt16 *src = (const SInt16 *)sampleBuf + firstSampleFrame * SOF_DMIC_CHANNELS;
	float *dst = (float *)destBuf;
	UInt32 n = numSampleFrames * SOF_DMIC_CHANNELS;
	const float scale = 1.0f / 32768.0f;

	for (UInt32 i = 0; i < n; i++)
		dst[i] = (float)src[i] * scale;
	return kIOReturnSuccess;
}

/*
 * No interrupts: poll the DMA position and, when it wrapped, take the time stamp of the wrap,
 * back-dated by the frames already written since then.
 */
void VoodooHDASofEngine::pollPosition()
{
	UInt32 frame = mSof->captureFrame();

	if (frame < mLastFrame) {
		uint64_t now, back, ts;
		clock_get_uptime(&now);
		nanoseconds_to_absolutetime((uint64_t)frame * 1000000000ULL / SOF_DMIC_RATE, &back);
		ts = now - back;
		takeTimeStamp(true, reinterpret_cast<AbsoluteTime *>(&ts));
		mWraps++;
	}
	mLastFrame = frame;
}

void VoodooHDASofEngine::posTimerFired(OSObject *owner, IOTimerEventSource *sender)
{
	VoodooHDASofEngine *self = OSDynamicCast(VoodooHDASofEngine, owner);

	if (!self || !self->mRunning)
		return;
	self->pollPosition();
	sender->setTimeoutMS(SOF_ENGINE_POLL_MS);
}
