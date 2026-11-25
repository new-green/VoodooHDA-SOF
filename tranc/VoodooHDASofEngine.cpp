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

#include "rnnoise/rnnoise_kern.h"

#define ENGLOG(fmt, ...)	IOLog("VoodooHDA SOF engine: " fmt "\n", ##__VA_ARGS__)

#define super IOAudioEngine
OSDefineMetaClassAndStructors(VoodooHDASofEngine, IOAudioEngine)

bool VoodooHDASofEngine::initWithSof(VoodooHDASof *sof, UInt32 nrMode)
{
	if (!super::init(NULL) || !sof || !sof->captureBuffer() || nrMode > kSofNrStrong)
		return false;
	mSof = sof;
	mSof->retain();
	mNrMode = nrMode;
	if (mNrMode == kSofNrOff)
		return true;
	/* RNNoise is a single static instance: only one noise-reduced engine may exist */
	mNrLock = IOLockAlloc();
	mNrIn = (float *)IOMalloc(RNNK_FRAME * sizeof(float));
	mNrBlk = (float *)IOMalloc(RNNK_FRAME * sizeof(float));
	mNrOut = (float *)IOMalloc(SOF_RING_FRAMES * sizeof(float));
	if (!mNrLock || !mNrIn || !mNrBlk || !mNrOut)
		return false;
	if (rnnk_init(mNrMode == kSofNrLight ? SOF_NR_LIGHT_FLOOR : 0.0f) != 0) {
		ENGLOG("error: RNNoise init failed");
		return false;
	}
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
	if (mNrIn)
		IOFree(mNrIn, RNNK_FRAME * sizeof(float));
	if (mNrBlk)
		IOFree(mNrBlk, RNNK_FRAME * sizeof(float));
	if (mNrOut)
		IOFree(mNrOut, SOF_RING_FRAMES * sizeof(float));
	mNrIn = mNrBlk = mNrOut = NULL;
	if (mNrLock) {
		IOLockFree(mNrLock);
		mNrLock = NULL;
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

	setDescription(mNrMode == kSofNrOff ? "Dahili Mikrofon" : "Dahili Mikrofon (Gürültü Azaltma)");
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
	ENGLOG("ready: 48 kHz, 2 ch, S16LE, %u frames%s", (unsigned)(SOF_DMIC_BUF_SIZE / SOF_DMIC_FRAME_BYTES),
	       mNrMode == kSofNrOff ? "" : mNrMode == kSofNrLight ? ", noise reduction light" : ", noise reduction strong");
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
	mVolCtl = vol;
	vol->release();

	mute = IOAudioToggleControl::createMuteControl(false, kIOAudioControlChannelIDAll,
			kIOAudioControlChannelNameAll, 0, kIOAudioControlUsageInput);
	if (!mute)
		return false;
	mute->setValueChangeHandler(&VoodooHDASofEngine::muteChanged, this);
	addDefaultAudioControl(mute);
	mMuteCtl = mute;
	mute->release();
	return true;
}

/* mirror a DSP-wide setting into the other engine's control (no handler call) */
static void syncPeerControl(IOAudioControl *ctl, SInt32 value)
{
	OSNumber *n;

	if (!ctl || !(n = OSNumber::withNumber((unsigned long long)(UInt32)value, 32)))
		return;
	ctl->hardwareValueChanged(n);
	n->release();
}

IOReturn VoodooHDASofEngine::volumeChanged(OSObject *target, IOAudioControl *, SInt32 oldValue, SInt32 newValue)
{
	VoodooHDASofEngine *self = OSDynamicCast(VoodooHDASofEngine, target);

	if (!self || newValue < 0)
		return kIOReturnBadArgument;
	ENGLOG("input level %d -> %d (%d dB)", (int)oldValue, (int)newValue, (int)newValue + SOF_VOL_MIN_DB);
	if (!self->mSof->setCaptureVolume((UInt32)newValue))
		return kIOReturnError;
	if (self->mPeer)
		syncPeerControl(self->mPeer->mVolCtl, newValue);
	return kIOReturnSuccess;
}

IOReturn VoodooHDASofEngine::muteChanged(OSObject *target, IOAudioControl *, SInt32 oldValue, SInt32 newValue)
{
	VoodooHDASofEngine *self = OSDynamicCast(VoodooHDASofEngine, target);

	if (!self)
		return kIOReturnBadArgument;
	ENGLOG("input mute %s", newValue ? "on" : "off");
	if (!self->mSof->setCaptureMute(newValue != 0))
		return kIOReturnError;
	if (self->mPeer)
		syncPeerControl(self->mPeer->mMuteCtl, newValue);
	return kIOReturnSuccess;
}

void VoodooHDASofEngine::stop(IOService *provider)
{
	if (mPeer) {
		mPeer->mPeer = NULL;
		mPeer = NULL;
	}
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
	mWraps = 0;
	if (mNrLock) {
		IOLockLock(mNrLock);
		mNrSync = false;
		mNrBlocks = 0;
		mLimHits = 0;
		IOLockUnlock(mNrLock);
	}
	/*
	 * The other engine may already be running the shared stream, so the ring is not
	 * necessarily at frame 0: back-date the start time stamp to the ring's frame 0.
	 */
	uint64_t now, back, ts;
	UInt32 frame = mSof->captureFrame();
	clock_get_uptime(&now);
	nanoseconds_to_absolutetime((uint64_t)frame * 1000000000ULL / SOF_DMIC_RATE, &back);
	ts = now - back;
	mLastFrame = frame;
	mRunning = true;
	takeTimeStamp(false, reinterpret_cast<AbsoluteTime *>(&ts));
	if (mPosTimer)
		mPosTimer->setTimeoutMS(SOF_ENGINE_POLL_MS);
	ENGLOG("started at ring frame %u", (unsigned)frame);
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
	ENGLOG("stopped after %u buffer wraps (%u denoised blocks, %u limiter hits)", (unsigned)mWraps,
	       (unsigned)mNrBlocks, (unsigned)mLimHits);
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

/*
 * Feed the denoiser with the ring frames [mNrInPos, endFrame). Input block [s, s+480) comes out
 * (RNNoise adds one frame of delay) at ring positions [s+480, s+960): every output frame then
 * only depends on input that is older than itself, which the HAL never reads ahead of.
 * Latency: +20 ms. Caller holds mNrLock.
 */
/* Caller holds mNrLock. RNNoise keeps its state; it adapts within a few frames. */
void VoodooHDASofEngine::nrReset()
{
	bzero(mNrOut, SOF_RING_FRAMES * sizeof(float));
	mNrFill = 0;
	mHpX1 = mHpX2 = mHpY1 = mHpY2 = 0;
	mLimGain = 1.0f;
	mNrSync = true;
}

void VoodooHDASofEngine::nrFeed(const SInt16 *ring, UInt32 endFrame)
{
	/* 2nd-order Butterworth high-pass, fc 90 Hz, fs 48 kHz (RBJ cookbook) */
	const double b0 = 0.991704195577, b1 = -1.983408391154, b2 = 0.991704195577;
	const double a1 = -1.983339569599, a2 = 0.983477212710;

	UInt32 todo = (endFrame + SOF_RING_FRAMES - mNrInPos) % SOF_RING_FRAMES;

	while (todo--) {
		const SInt16 *f = ring + mNrInPos * SOF_DMIC_CHANNELS;
		if (mNrFill == 0)
			mNrBlockStart = mNrInPos;
		double x = 0.5 * ((double)f[0] + (double)f[1]);		/* RNNoise works in 16-bit scale */
		double y = b0 * x + b1 * mHpX1 + b2 * mHpX2 - a1 * mHpY1 - a2 * mHpY2;
		mHpX2 = mHpX1; mHpX1 = x;
		mHpY2 = mHpY1; mHpY1 = y;
		mNrIn[mNrFill++] = (float)y;
		mNrInPos = (mNrInPos + 1) % SOF_RING_FRAMES;
		if (mNrFill == RNNK_FRAME) {
			rnnk_process(mNrBlk, mNrIn);
			UInt32 o = (mNrBlockStart + RNNK_FRAME) % SOF_RING_FRAMES;
			for (UInt32 i = 0; i < RNNK_FRAME; i++) {
				/* peak limiter: instant attack, 100 ms release */
				float v = mNrBlk[i], a = v < 0 ? -v : v;
				if (a * mLimGain > SOF_NR_LIMIT) {
					mLimGain = SOF_NR_LIMIT / a;
					mLimHits++;
				} else {
					mLimGain += (1.0f - mLimGain) * SOF_NR_LIMIT_RELEASE;
				}
				mNrOut[(o + i) % SOF_RING_FRAMES] = v * mLimGain;
			}
			mNrFill = 0;
			mNrBlocks++;
		}
	}
}

IOReturn VoodooHDASofEngine::convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame,
		UInt32 numSampleFrames, const IOAudioStreamFormat *, IOAudioStream *)
{
	const SInt16 *src = (const SInt16 *)sampleBuf + firstSampleFrame * SOF_DMIC_CHANNELS;
	float *dst = (float *)destBuf;
	UInt32 n = numSampleFrames * SOF_DMIC_CHANNELS;
	const float scale = 1.0f / 32768.0f;

	if (mNrMode == kSofNrOff || !mNrLock || firstSampleFrame >= SOF_RING_FRAMES || numSampleFrames > SOF_RING_FRAMES / 2) {
		for (UInt32 i = 0; i < n; i++)
			dst[i] = (float)src[i] * scale;
		return kIOReturnSuccess;
	}

	IOLockLock(mNrLock);
	UInt32 end = (firstSampleFrame + numSampleFrames) % SOF_RING_FRAMES;
	if (!mNrSync || (end + SOF_RING_FRAMES - mNrInPos) % SOF_RING_FRAMES > SOF_NR_RESYNC_FRAMES) {
		/* (re)start: output stays silent until the first block lands (<= 20 ms) */
		mNrInPos = firstSampleFrame;
		nrReset();
	}
	nrFeed((const SInt16 *)sampleBuf, end);
	for (UInt32 i = 0; i < numSampleFrames; i++) {
		float v = mNrOut[(firstSampleFrame + i) % SOF_RING_FRAMES] * scale;
		dst[2 * i] = v;
		dst[2 * i + 1] = v;
	}
	IOLockUnlock(mNrLock);
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
