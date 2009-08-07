/* //device/include/server/AudioFlinger/AudioFlinger.cpp
**
** Copyright 2007, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/


#define LOG_TAG "AudioFlinger"
//#define LOG_NDEBUG 0

#include <math.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <binder/Parcel.h>
#include <binder/IPCThreadState.h>
#include <utils/String16.h>
#include <utils/threads.h>

#include <cutils/properties.h>

#include <media/AudioTrack.h>
#include <media/AudioRecord.h>

#include <private/media/AudioTrackShared.h>

#include <hardware_legacy/AudioHardwareInterface.h>

#include "AudioMixer.h"
#include "AudioFlinger.h"

#ifdef WITH_A2DP
#include "A2dpAudioInterface.h"
#endif

// ----------------------------------------------------------------------------
// the sim build doesn't have gettid

#ifndef HAVE_GETTID
# define gettid getpid
#endif

// ----------------------------------------------------------------------------

namespace android {

static const char* kDeadlockedString = "AudioFlinger may be deadlocked\n";
static const char* kHardwareLockedString = "Hardware lock is taken\n";

//static const nsecs_t kStandbyTimeInNsecs = seconds(3);
static const unsigned long kBufferRecoveryInUsecs = 2000;
static const unsigned long kMaxBufferRecoveryInUsecs = 20000;
static const float MAX_GAIN = 4096.0f;

// retry counts for buffer fill timeout
// 50 * ~20msecs = 1 second
static const int8_t kMaxTrackRetries = 50;
static const int8_t kMaxTrackStartupRetries = 50;

static const int kDumpLockRetries = 50;
static const int kDumpLockSleep = 20000;


#define AUDIOFLINGER_SECURITY_ENABLED 1

// ----------------------------------------------------------------------------

static bool recordingAllowed() {
#ifndef HAVE_ANDROID_OS
    return true;
#endif
#if AUDIOFLINGER_SECURITY_ENABLED
    if (getpid() == IPCThreadState::self()->getCallingPid()) return true;
    bool ok = checkCallingPermission(String16("android.permission.RECORD_AUDIO"));
    if (!ok) LOGE("Request requires android.permission.RECORD_AUDIO");
    return ok;
#else
    if (!checkCallingPermission(String16("android.permission.RECORD_AUDIO")))
        LOGW("WARNING: Need to add android.permission.RECORD_AUDIO to manifest");
    return true;
#endif
}

static bool settingsAllowed() {
#ifndef HAVE_ANDROID_OS
    return true;
#endif
#if AUDIOFLINGER_SECURITY_ENABLED
    if (getpid() == IPCThreadState::self()->getCallingPid()) return true;
    bool ok = checkCallingPermission(String16("android.permission.MODIFY_AUDIO_SETTINGS"));
    if (!ok) LOGE("Request requires android.permission.MODIFY_AUDIO_SETTINGS");
    return ok;
#else
    if (!checkCallingPermission(String16("android.permission.MODIFY_AUDIO_SETTINGS")))
        LOGW("WARNING: Need to add android.permission.MODIFY_AUDIO_SETTINGS to manifest");
    return true;
#endif
}

// ----------------------------------------------------------------------------

AudioFlinger::AudioFlinger()
    : BnAudioFlinger(),
        mAudioHardware(0), mMasterVolume(1.0f), mMasterMute(false), mNextThreadId(0)
{
    mHardwareStatus = AUDIO_HW_IDLE;

    mAudioHardware = AudioHardwareInterface::create();

    mHardwareStatus = AUDIO_HW_INIT;
    if (mAudioHardware->initCheck() == NO_ERROR) {
        // open 16-bit output stream for s/w mixer

        setMode(AudioSystem::MODE_NORMAL);

        setMasterVolume(1.0f);
        setMasterMute(false);
    } else {
        LOGE("Couldn't even initialize the stubbed audio hardware!");
    }
}

AudioFlinger::~AudioFlinger()
{
    mRecordThreads.clear();
    mPlaybackThreads.clear();
}



status_t AudioFlinger::dumpClients(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    result.append("Clients:\n");
    for (size_t i = 0; i < mClients.size(); ++i) {
        wp<Client> wClient = mClients.valueAt(i);
        if (wClient != 0) {
            sp<Client> client = wClient.promote();
            if (client != 0) {
                snprintf(buffer, SIZE, "  pid: %d\n", client->pid());
                result.append(buffer);
            }
        }
    }
    write(fd, result.string(), result.size());
    return NO_ERROR;
}


status_t AudioFlinger::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    int hardwareStatus = mHardwareStatus;

    snprintf(buffer, SIZE, "Hardware status: %d\n", hardwareStatus);
    result.append(buffer);
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioFlinger::dumpPermissionDenial(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, SIZE, "Permission Denial: "
            "can't dump AudioFlinger from pid=%d, uid=%d\n",
            IPCThreadState::self()->getCallingPid(),
            IPCThreadState::self()->getCallingUid());
    result.append(buffer);
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

static bool tryLock(Mutex& mutex)
{
    bool locked = false;
    for (int i = 0; i < kDumpLockRetries; ++i) {
        if (mutex.tryLock() == NO_ERROR) {
            locked = true;
            break;
        }
        usleep(kDumpLockSleep);
    }
    return locked;
}

status_t AudioFlinger::dump(int fd, const Vector<String16>& args)
{
    if (checkCallingPermission(String16("android.permission.DUMP")) == false) {
        dumpPermissionDenial(fd, args);
    } else {
        // get state of hardware lock
        bool hardwareLocked = tryLock(mHardwareLock);
        if (!hardwareLocked) {
            String8 result(kHardwareLockedString);
            write(fd, result.string(), result.size());
        } else {
            mHardwareLock.unlock();
        }

        bool locked = tryLock(mLock);

        // failed to lock - AudioFlinger is probably deadlocked
        if (!locked) {
            String8 result(kDeadlockedString);
            write(fd, result.string(), result.size());
        }

        dumpClients(fd, args);
        dumpInternals(fd, args);

        // dump playback threads
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            mPlaybackThreads.valueAt(i)->dump(fd, args);
        }

        // dump record threads
        for (size_t i = 0; i < mRecordThreads.size(); i++) {
            mRecordThreads.valueAt(i)->dump(fd, args);
        }

        if (mAudioHardware) {
            mAudioHardware->dumpState(fd, args);
        }
        if (locked) mLock.unlock();
    }
    return NO_ERROR;
}


// IAudioFlinger interface


sp<IAudioTrack> AudioFlinger::createTrack(
        pid_t pid,
        int streamType,
        uint32_t sampleRate,
        int format,
        int channelCount,
        int frameCount,
        uint32_t flags,
        const sp<IMemory>& sharedBuffer,
        int output,
        status_t *status)
{
    sp<PlaybackThread::Track> track;
    sp<TrackHandle> trackHandle;
    sp<Client> client;
    wp<Client> wclient;
    status_t lStatus;

    if (streamType >= AudioSystem::NUM_STREAM_TYPES) {
        LOGE("invalid stream type");
        lStatus = BAD_VALUE;
        goto Exit;
    }

    {
        Mutex::Autolock _l(mLock);
        PlaybackThread *thread = checkPlaybackThread_l(output);
        if (thread == NULL) {
            LOGE("unknown output thread");
            lStatus = BAD_VALUE;
            goto Exit;
        }

        wclient = mClients.valueFor(pid);

        if (wclient != NULL) {
            client = wclient.promote();
        } else {
            client = new Client(this, pid);
            mClients.add(pid, client);
        }
        track = thread->createTrack_l(client, streamType, sampleRate, format,
                channelCount, frameCount, sharedBuffer, &lStatus);
    }
    if (lStatus == NO_ERROR) {
        trackHandle = new TrackHandle(track);
    } else {
        track.clear();
    }

Exit:
    if(status) {
        *status = lStatus;
    }
    return trackHandle;
}

uint32_t AudioFlinger::sampleRate(int output) const
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        LOGW("sampleRate() unknown thread %d", output);
        return 0;
    }
    return thread->sampleRate();
}

int AudioFlinger::channelCount(int output) const
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        LOGW("channelCount() unknown thread %d", output);
        return 0;
    }
    return thread->channelCount();
}

int AudioFlinger::format(int output) const
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        LOGW("format() unknown thread %d", output);
        return 0;
    }
    return thread->format();
}

size_t AudioFlinger::frameCount(int output) const
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        LOGW("frameCount() unknown thread %d", output);
        return 0;
    }
    return thread->frameCount();
}

uint32_t AudioFlinger::latency(int output) const
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        LOGW("latency() unknown thread %d", output);
        return 0;
    }
    return thread->latency();
}

status_t AudioFlinger::setMasterVolume(float value)
{
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    // when hw supports master volume, don't scale in sw mixer
    AutoMutex lock(mHardwareLock);
    mHardwareStatus = AUDIO_HW_SET_MASTER_VOLUME;
    if (mAudioHardware->setMasterVolume(value) == NO_ERROR) {
        value = 1.0f;
    }
    mHardwareStatus = AUDIO_HW_IDLE;

    mMasterVolume = value;
    for (uint32_t i = 0; i < mPlaybackThreads.size(); i++)
       mPlaybackThreads.valueAt(i)->setMasterVolume(value);

    return NO_ERROR;
}

status_t AudioFlinger::setMode(int mode)
{
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if ((mode < 0) || (mode >= AudioSystem::NUM_MODES)) {
        LOGW("Illegal value: setMode(%d)", mode);
        return BAD_VALUE;
    }

    AutoMutex lock(mHardwareLock);
    mHardwareStatus = AUDIO_HW_SET_MODE;
    status_t ret = mAudioHardware->setMode(mode);
    mHardwareStatus = AUDIO_HW_IDLE;
    return ret;
}

status_t AudioFlinger::setMicMute(bool state)
{
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    AutoMutex lock(mHardwareLock);
    mHardwareStatus = AUDIO_HW_SET_MIC_MUTE;
    status_t ret = mAudioHardware->setMicMute(state);
    mHardwareStatus = AUDIO_HW_IDLE;
    return ret;
}

bool AudioFlinger::getMicMute() const
{
    bool state = AudioSystem::MODE_INVALID;
    mHardwareStatus = AUDIO_HW_GET_MIC_MUTE;
    mAudioHardware->getMicMute(&state);
    mHardwareStatus = AUDIO_HW_IDLE;
    return state;
}

status_t AudioFlinger::setMasterMute(bool muted)
{
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    mMasterMute = muted;
    for (uint32_t i = 0; i < mPlaybackThreads.size(); i++)
       mPlaybackThreads.valueAt(i)->setMasterMute(muted);

    return NO_ERROR;
}

float AudioFlinger::masterVolume() const
{
    return mMasterVolume;
}

bool AudioFlinger::masterMute() const
{
    return mMasterMute;
}

status_t AudioFlinger::setStreamVolume(int stream, float value, int output)
{
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    if (stream < 0 || uint32_t(stream) >= AudioSystem::NUM_STREAM_TYPES) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    PlaybackThread *thread = NULL;
    if (output) {
        thread = checkPlaybackThread_l(output);
        if (thread == NULL) {
            return BAD_VALUE;
        }
    }

    status_t ret = NO_ERROR;

    if (stream == AudioSystem::VOICE_CALL ||
        stream == AudioSystem::BLUETOOTH_SCO) {
        float hwValue;
        if (stream == AudioSystem::VOICE_CALL) {
            hwValue = (float)AudioSystem::logToLinear(value)/100.0f;
            // offset value to reflect actual hardware volume that never reaches 0
            // 1% corresponds roughly to first step in VOICE_CALL stream volume setting (see AudioService.java)
            value = 0.01 + 0.99 * value;
        } else { // (type == AudioSystem::BLUETOOTH_SCO)
            hwValue = 1.0f;
        }

        AutoMutex lock(mHardwareLock);
        mHardwareStatus = AUDIO_SET_VOICE_VOLUME;
        ret = mAudioHardware->setVoiceVolume(hwValue);
        mHardwareStatus = AUDIO_HW_IDLE;

    }

    mStreamTypes[stream].volume = value;

    if (thread == NULL) {
        for (uint32_t i = 0; i < mPlaybackThreads.size(); i++)
           mPlaybackThreads.valueAt(i)->setStreamVolume(stream, value);

    } else {
        thread->setStreamVolume(stream, value);
    }

    return ret;
}

status_t AudioFlinger::setStreamMute(int stream, bool muted)
{
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    if (stream < 0 || uint32_t(stream) >= AudioSystem::NUM_STREAM_TYPES ||
        uint32_t(stream) == AudioSystem::ENFORCED_AUDIBLE) {
        return BAD_VALUE;
    }

    mStreamTypes[stream].mute = muted;
    for (uint32_t i = 0; i < mPlaybackThreads.size(); i++)
       mPlaybackThreads.valueAt(i)->setStreamMute(stream, muted);

    return NO_ERROR;
}

float AudioFlinger::streamVolume(int stream, int output) const
{
    if (stream < 0 || uint32_t(stream) >= AudioSystem::NUM_STREAM_TYPES) {
        return 0.0f;
    }

    AutoMutex lock(mLock);
    float volume;
    if (output) {
        PlaybackThread *thread = checkPlaybackThread_l(output);
        if (thread == NULL) {
            return 0.0f;
        }
        volume = thread->streamVolume(stream);
    } else {
        volume = mStreamTypes[stream].volume;
    }

    // remove correction applied by setStreamVolume()
    if (stream == AudioSystem::VOICE_CALL) {
        volume = (volume - 0.01) / 0.99 ;
    }

    return volume;
}

bool AudioFlinger::streamMute(int stream) const
{
    if (stream < 0 || stream >= (int)AudioSystem::NUM_STREAM_TYPES) {
        return true;
    }

    return mStreamTypes[stream].mute;
}

bool AudioFlinger::isMusicActive() const
{
    Mutex::Autolock _l(mLock);
    for (uint32_t i = 0; i < mPlaybackThreads.size(); i++) {
        if (mPlaybackThreads.valueAt(i)->isMusicActive()) {
            return true;
        }
    }
    return false;
}

status_t AudioFlinger::setParameters(int ioHandle, const String8& keyValuePairs)
{
    status_t result;

    LOGV("setParameters(): io %d, keyvalue %s, tid %d, calling tid %d",
            ioHandle, keyValuePairs.string(), gettid(), IPCThreadState::self()->getCallingPid());
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    // ioHandle == 0 means the parameters are global to the audio hardware interface
    if (ioHandle == 0) {
        AutoMutex lock(mHardwareLock);
        mHardwareStatus = AUDIO_SET_PARAMETER;
        result = mAudioHardware->setParameters(keyValuePairs);
        mHardwareStatus = AUDIO_HW_IDLE;
        return result;
    }

    // Check if parameters are for an output
    PlaybackThread *playbackThread = checkPlaybackThread_l(ioHandle);
    if (playbackThread != NULL) {
        return playbackThread->setParameters(keyValuePairs);
    }

    // Check if parameters are for an input
    RecordThread *recordThread = checkRecordThread_l(ioHandle);
    if (recordThread != NULL) {
        return recordThread->setParameters(keyValuePairs);
    }

    return BAD_VALUE;
}

String8 AudioFlinger::getParameters(int ioHandle, const String8& keys)
{
//    LOGV("getParameters() io %d, keys %s, tid %d, calling tid %d",
//            ioHandle, keys.string(), gettid(), IPCThreadState::self()->getCallingPid());

    if (ioHandle == 0) {
        return mAudioHardware->getParameters(keys);
    }
    PlaybackThread *playbackThread = checkPlaybackThread_l(ioHandle);
    if (playbackThread != NULL) {
        return playbackThread->getParameters(keys);
    }
    RecordThread *recordThread = checkRecordThread_l(ioHandle);
    if (recordThread != NULL) {
        return recordThread->getParameters(keys);
    }
    return String8("");
}

size_t AudioFlinger::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    return mAudioHardware->getInputBufferSize(sampleRate, format, channelCount);
}

void AudioFlinger::registerClient(const sp<IAudioFlingerClient>& client)
{

    LOGV("registerClient() %p, tid %d, calling tid %d", client.get(), gettid(), IPCThreadState::self()->getCallingPid());
    Mutex::Autolock _l(mLock);

    sp<IBinder> binder = client->asBinder();
    if (mNotificationClients.indexOf(binder) < 0) {
        LOGV("Adding notification client %p", binder.get());
        binder->linkToDeath(this);
        mNotificationClients.add(binder);
    }

    // the config change is always sent from playback or record threads to avoid deadlock
    // with AudioSystem::gLock
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        mPlaybackThreads.valueAt(i)->sendConfigEvent(AudioSystem::OUTPUT_OPENED);
    }

    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        mRecordThreads.valueAt(i)->sendConfigEvent(AudioSystem::INPUT_OPENED);
    }
}

void AudioFlinger::binderDied(const wp<IBinder>& who) {

    LOGV("binderDied() %p, tid %d, calling tid %d", who.unsafe_get(), gettid(), IPCThreadState::self()->getCallingPid());
    Mutex::Autolock _l(mLock);

    IBinder *binder = who.unsafe_get();

    if (binder != NULL) {
        int index = mNotificationClients.indexOf(binder);
        if (index >= 0) {
            LOGV("Removing notification client %p", binder);
            mNotificationClients.removeAt(index);
        }
    }
}

void AudioFlinger::audioConfigChanged(int event, const sp<ThreadBase>& thread, void *param2) {
    Mutex::Autolock _l(mLock);
    int ioHandle = 0;

    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        if (mPlaybackThreads.valueAt(i) == thread) {
            ioHandle = mPlaybackThreads.keyAt(i);
            break;
        }
    }
    if (ioHandle == 0) {
        for (size_t i = 0; i < mRecordThreads.size(); i++) {
            if (mRecordThreads.valueAt(i) == thread) {
                ioHandle = mRecordThreads.keyAt(i);
                break;
            }
        }
    }

    if (ioHandle != 0) {
        size_t size = mNotificationClients.size();
        for (size_t i = 0; i < size; i++) {
            sp<IBinder> binder = mNotificationClients.itemAt(i);
            LOGV("audioConfigChanged() Notifying change to client %p", binder.get());
            sp<IAudioFlingerClient> client = interface_cast<IAudioFlingerClient> (binder);
            client->ioConfigChanged(event, ioHandle, param2);
        }
    }
}

void AudioFlinger::removeClient(pid_t pid)
{
    LOGV("removeClient() pid %d, tid %d, calling tid %d", pid, gettid(), IPCThreadState::self()->getCallingPid());
    Mutex::Autolock _l(mLock);
    mClients.removeItem(pid);
}

// ----------------------------------------------------------------------------

AudioFlinger::ThreadBase::ThreadBase(const sp<AudioFlinger>& audioFlinger)
    :   Thread(false),
        mAudioFlinger(audioFlinger), mSampleRate(0), mFrameCount(0), mChannelCount(0),
        mFormat(0), mFrameSize(1), mStandby(false)
{
}

AudioFlinger::ThreadBase::~ThreadBase()
{
    mParamCond.broadcast();
    mNewParameters.clear();
}

void AudioFlinger::ThreadBase::exit()
{
    // keep a strong ref on ourself so that we want get
    // destroyed in the middle of requestExitAndWait()
    sp <ThreadBase> strongMe = this;

    LOGV("ThreadBase::exit");
    {
        AutoMutex lock(&mLock);
        requestExit();
        mWaitWorkCV.signal();
    }
    requestExitAndWait();
}

uint32_t AudioFlinger::ThreadBase::sampleRate() const
{
    return mSampleRate;
}

int AudioFlinger::ThreadBase::channelCount() const
{
    return mChannelCount;
}

int AudioFlinger::ThreadBase::format() const
{
    return mFormat;
}

size_t AudioFlinger::ThreadBase::frameCount() const
{
    return mFrameCount;
}

status_t AudioFlinger::ThreadBase::setParameters(const String8& keyValuePairs)
{
    status_t status;

    LOGV("ThreadBase::setParameters() %s", keyValuePairs.string());
    Mutex::Autolock _l(mLock);

    mNewParameters.add(keyValuePairs);
    mWaitWorkCV.signal();
    mParamCond.wait(mLock);
    status = mParamStatus;
    mWaitWorkCV.signal();
    return status;
}

void AudioFlinger::ThreadBase::sendConfigEvent(int event, int param)
{
    Mutex::Autolock _l(mLock);
    sendConfigEvent_l(event, param);
}

// sendConfigEvent_l() must be called with ThreadBase::mLock held
void AudioFlinger::ThreadBase::sendConfigEvent_l(int event, int param)
{
    ConfigEvent *configEvent = new ConfigEvent();
    configEvent->mEvent = event;
    configEvent->mParam = param;
    mConfigEvents.add(configEvent);
    LOGV("sendConfigEvent() num events %d event %d, param %d", mConfigEvents.size(), event, param);
    mWaitWorkCV.signal();
}

void AudioFlinger::ThreadBase::processConfigEvents()
{
    mLock.lock();
    while(!mConfigEvents.isEmpty()) {
        LOGV("processConfigEvents() remaining events %d", mConfigEvents.size());
        ConfigEvent *configEvent = mConfigEvents[0];
        mConfigEvents.removeAt(0);
        // release mLock because audioConfigChanged() will call
        // Audioflinger::audioConfigChanged() which locks AudioFlinger mLock thus creating
        // potential cross deadlock between AudioFlinger::mLock and mLock
        mLock.unlock();
        audioConfigChanged(configEvent->mEvent, configEvent->mParam);
        delete configEvent;
        mLock.lock();
    }
    mLock.unlock();
}


// ----------------------------------------------------------------------------

AudioFlinger::PlaybackThread::PlaybackThread(const sp<AudioFlinger>& audioFlinger, AudioStreamOut* output)
    :   ThreadBase(audioFlinger),
        mMixBuffer(0), mSuspended(false), mBytesWritten(0), mOutput(output),
        mLastWriteTime(0), mNumWrites(0), mNumDelayedWrites(0), mInWrite(false)
{
    readOutputParameters();

    mMasterVolume = mAudioFlinger->masterVolume();
    mMasterMute = mAudioFlinger->masterMute();

    for (int stream = 0; stream < AudioSystem::NUM_STREAM_TYPES; stream++) {
        mStreamTypes[stream].volume = mAudioFlinger->streamVolumeInternal(stream);
        mStreamTypes[stream].mute = mAudioFlinger->streamMute(stream);
    }
    // notify client processes that a new input has been opened
    sendConfigEvent(AudioSystem::OUTPUT_OPENED);
}

AudioFlinger::PlaybackThread::~PlaybackThread()
{
    delete [] mMixBuffer;
    if (mType != DUPLICATING) {
        mAudioFlinger->mAudioHardware->closeOutputStream(mOutput);
    }
}

status_t AudioFlinger::PlaybackThread::dump(int fd, const Vector<String16>& args)
{
    dumpInternals(fd, args);
    dumpTracks(fd, args);
    return NO_ERROR;
}

status_t AudioFlinger::PlaybackThread::dumpTracks(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "Output thread %p tracks\n", this);
    result.append(buffer);
    result.append("   Name Clien Typ Fmt Chn Buf S M F SRate LeftV RighV Serv User\n");
    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<Track> track = mTracks[i];
        if (track != 0) {
            track->dump(buffer, SIZE);
            result.append(buffer);
        }
    }

    snprintf(buffer, SIZE, "Output thread %p active tracks\n", this);
    result.append(buffer);
    result.append("   Name Clien Typ Fmt Chn Buf S M F SRate LeftV RighV Serv User\n");
    for (size_t i = 0; i < mActiveTracks.size(); ++i) {
        wp<Track> wTrack = mActiveTracks[i];
        if (wTrack != 0) {
            sp<Track> track = wTrack.promote();
            if (track != 0) {
                track->dump(buffer, SIZE);
                result.append(buffer);
            }
        }
    }
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioFlinger::PlaybackThread::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "Output thread %p internals\n", this);
    result.append(buffer);
    snprintf(buffer, SIZE, "last write occurred (msecs): %llu\n", ns2ms(systemTime() - mLastWriteTime));
    result.append(buffer);
    snprintf(buffer, SIZE, "total writes: %d\n", mNumWrites);
    result.append(buffer);
    snprintf(buffer, SIZE, "delayed writes: %d\n", mNumDelayedWrites);
    result.append(buffer);
    snprintf(buffer, SIZE, "blocked in write: %d\n", mInWrite);
    result.append(buffer);
    snprintf(buffer, SIZE, "standby: %d\n", mStandby);
    result.append(buffer);
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

// Thread virtuals
status_t AudioFlinger::PlaybackThread::readyToRun()
{
    if (mSampleRate == 0) {
        LOGE("No working audio driver found.");
        return NO_INIT;
    }
    LOGI("AudioFlinger's thread %p ready to run", this);
    return NO_ERROR;
}

void AudioFlinger::PlaybackThread::onFirstRef()
{
    const size_t SIZE = 256;
    char buffer[SIZE];

    snprintf(buffer, SIZE, "Playback Thread %p", this);

    run(buffer, ANDROID_PRIORITY_URGENT_AUDIO);
}

// PlaybackThread::createTrack_l() must be called with AudioFlinger::mLock held
sp<AudioFlinger::PlaybackThread::Track>  AudioFlinger::PlaybackThread::createTrack_l(
        const sp<AudioFlinger::Client>& client,
        int streamType,
        uint32_t sampleRate,
        int format,
        int channelCount,
        int frameCount,
        const sp<IMemory>& sharedBuffer,
        status_t *status)
{
    sp<Track> track;
    status_t lStatus;

    if (mType == DIRECT) {
        if (sampleRate != mSampleRate || format != mFormat || channelCount != mChannelCount) {
            LOGE("createTrack_l() Bad parameter:  sampleRate %d format %d, channelCount %d for output %p",
                 sampleRate, format, channelCount, mOutput);
            lStatus = BAD_VALUE;
            goto Exit;
        }
    } else {
        // Resampler implementation limits input sampling rate to 2 x output sampling rate.
        if (sampleRate > mSampleRate*2) {
            LOGE("Sample rate out of range: %d mSampleRate %d", sampleRate, mSampleRate);
            lStatus = BAD_VALUE;
            goto Exit;
        }
    }

    if (mOutput == 0) {
        LOGE("Audio driver not initialized.");
        lStatus = NO_INIT;
        goto Exit;
    }

    { // scope for mLock
        Mutex::Autolock _l(mLock);
        track = new Track(this, client, streamType, sampleRate, format,
                channelCount, frameCount, sharedBuffer);
        if (track->getCblk() == NULL) {
            lStatus = NO_MEMORY;
            goto Exit;
        }
        mTracks.add(track);
    }
    lStatus = NO_ERROR;

Exit:
    if(status) {
        *status = lStatus;
    }
    return track;
}

uint32_t AudioFlinger::PlaybackThread::latency() const
{
    if (mOutput) {
        return mOutput->latency();
    }
    else {
        return 0;
    }
}

status_t AudioFlinger::PlaybackThread::setMasterVolume(float value)
{
    mMasterVolume = value;
    return NO_ERROR;
}

status_t AudioFlinger::PlaybackThread::setMasterMute(bool muted)
{
    mMasterMute = muted;
    return NO_ERROR;
}

float AudioFlinger::PlaybackThread::masterVolume() const
{
    return mMasterVolume;
}

bool AudioFlinger::PlaybackThread::masterMute() const
{
    return mMasterMute;
}

status_t AudioFlinger::PlaybackThread::setStreamVolume(int stream, float value)
{
    mStreamTypes[stream].volume = value;
    return NO_ERROR;
}

status_t AudioFlinger::PlaybackThread::setStreamMute(int stream, bool muted)
{
    mStreamTypes[stream].mute = muted;
    return NO_ERROR;
}

float AudioFlinger::PlaybackThread::streamVolume(int stream) const
{
    return mStreamTypes[stream].volume;
}

bool AudioFlinger::PlaybackThread::streamMute(int stream) const
{
    return mStreamTypes[stream].mute;
}

bool AudioFlinger::PlaybackThread::isMusicActive() const
{
    Mutex::Autolock _l(mLock);
    size_t count = mActiveTracks.size();
    for (size_t i = 0 ; i < count ; ++i) {
        sp<Track> t = mActiveTracks[i].promote();
        if (t == 0) continue;
        Track* const track = t.get();
        if (t->type() == AudioSystem::MUSIC)
            return true;
    }
    return false;
}

// addTrack_l() must be called with ThreadBase::mLock held
status_t AudioFlinger::PlaybackThread::addTrack_l(const sp<Track>& track)
{
    status_t status = ALREADY_EXISTS;

    // here the track could be either new, or restarted
    // in both cases "unstop" the track
    if (track->isPaused()) {
        track->mState = TrackBase::RESUMING;
        LOGV("PAUSED => RESUMING (%d)", track->name());
    } else {
        track->mState = TrackBase::ACTIVE;
        LOGV("? => ACTIVE (%d)", track->name());
    }
    // set retry count for buffer fill
    track->mRetryCount = kMaxTrackStartupRetries;
    if (mActiveTracks.indexOf(track) < 0) {
        // the track is newly added, make sure it fills up all its
        // buffers before playing. This is to ensure the client will
        // effectively get the latency it requested.
        track->mFillingUpStatus = Track::FS_FILLING;
        track->mResetDone = false;
        mActiveTracks.add(track);
        status = NO_ERROR;
    }

    LOGV("mWaitWorkCV.broadcast");
    mWaitWorkCV.broadcast();

    return status;
}

// destroyTrack_l() must be called with ThreadBase::mLock held
void AudioFlinger::PlaybackThread::destroyTrack_l(const sp<Track>& track)
{
    track->mState = TrackBase::TERMINATED;
    if (mActiveTracks.indexOf(track) < 0) {
        LOGV("remove track (%d) and delete from mixer", track->name());
        mTracks.remove(track);
        deleteTrackName_l(track->name());
    }
}

String8 AudioFlinger::PlaybackThread::getParameters(const String8& keys)
{
    return mOutput->getParameters(keys);
}

void AudioFlinger::PlaybackThread::audioConfigChanged(int event, int param) {
    AudioSystem::OutputDescriptor desc;
    void *param2 = 0;

    LOGV("PlaybackThread::audioConfigChanged, thread %p, event %d, param %d", this, event, param);

    switch (event) {
    case AudioSystem::OUTPUT_OPENED:
    case AudioSystem::OUTPUT_CONFIG_CHANGED:
        desc.channels = mChannelCount;
        desc.samplingRate = mSampleRate;
        desc.format = mFormat;
        desc.frameCount = mFrameCount;
        desc.latency = latency();
        param2 = &desc;
        break;

    case AudioSystem::STREAM_CONFIG_CHANGED:
        param2 = &param;
    case AudioSystem::OUTPUT_CLOSED:
    default:
        break;
    }
    mAudioFlinger->audioConfigChanged(event, this, param2);
}

void AudioFlinger::PlaybackThread::readOutputParameters()
{
    mSampleRate = mOutput->sampleRate();
    mChannelCount = AudioSystem::popCount(mOutput->channels());

    mFormat = mOutput->format();
    mFrameSize = mOutput->frameSize();
    mFrameCount = mOutput->bufferSize() / mFrameSize;

    mMinBytesToWrite = (mOutput->latency() * mSampleRate * mFrameSize) / 1000;
    // FIXME - Current mixer implementation only supports stereo output: Always
    // Allocate a stereo buffer even if HW output is mono.
    if (mMixBuffer != NULL) delete mMixBuffer;
    mMixBuffer = new int16_t[mFrameCount * 2];
    memset(mMixBuffer, 0, mFrameCount * 2 * sizeof(int16_t));
}

// ----------------------------------------------------------------------------

AudioFlinger::MixerThread::MixerThread(const sp<AudioFlinger>& audioFlinger, AudioStreamOut* output)
    :   PlaybackThread(audioFlinger, output),
        mAudioMixer(0)
{
    mType = PlaybackThread::MIXER;
    mAudioMixer = new AudioMixer(mFrameCount, mSampleRate);

    // FIXME - Current mixer implementation only supports stereo output
    if (mChannelCount == 1) {
        LOGE("Invalid audio hardware channel count");
    }
}

AudioFlinger::MixerThread::~MixerThread()
{
    delete mAudioMixer;
}

bool AudioFlinger::MixerThread::threadLoop()
{
    unsigned long sleepTime = kBufferRecoveryInUsecs;
    int16_t* curBuf = mMixBuffer;
    Vector< sp<Track> > tracksToRemove;
    size_t enabledTracks = 0;
    nsecs_t standbyTime = systemTime();
    size_t mixBufferSize = mFrameCount * mFrameSize;
    nsecs_t maxPeriod = seconds(mFrameCount) / mSampleRate * 2;

    while (!exitPending())
    {
        processConfigEvents();

        enabledTracks = 0;
        { // scope for mLock

            Mutex::Autolock _l(mLock);

            if (checkForNewParameters_l()) {
                mixBufferSize = mFrameCount * mFrameSize;
                maxPeriod = seconds(mFrameCount) / mSampleRate * 2;
            }

            const SortedVector< wp<Track> >& activeTracks = mActiveTracks;

            // put audio hardware into standby after short delay
            if UNLIKELY((!activeTracks.size() && systemTime() > standbyTime) ||
                        mSuspended) {
                if (!mStandby) {
                    LOGV("Audio hardware entering standby, mixer %p, mSuspended %d\n", this, mSuspended);
                    mOutput->standby();
                    mStandby = true;
                    mBytesWritten = 0;
                }

                if (!activeTracks.size() && mConfigEvents.isEmpty()) {
                    // we're about to wait, flush the binder command buffer
                    IPCThreadState::self()->flushCommands();

                    if (exitPending()) break;

                    // wait until we have something to do...
                    LOGV("MixerThread %p TID %d going to sleep\n", this, gettid());
                    mWaitWorkCV.wait(mLock);
                    LOGV("MixerThread %p TID %d waking up\n", this, gettid());

                    if (mMasterMute == false) {
                        char value[PROPERTY_VALUE_MAX];
                        property_get("ro.audio.silent", value, "0");
                        if (atoi(value)) {
                            LOGD("Silence is golden");
                            setMasterMute(true);
                        }
                    }

                    standbyTime = systemTime() + kStandbyTimeInNsecs;
                    continue;
                }
            }

            enabledTracks = prepareTracks_l(activeTracks, &tracksToRemove);
       }

        if (LIKELY(enabledTracks)) {
            // mix buffers...
            mAudioMixer->process(curBuf);

            // output audio to hardware
            if (mSuspended) {
                usleep(kMaxBufferRecoveryInUsecs);
            } else {
                mLastWriteTime = systemTime();
                mInWrite = true;
                int bytesWritten = (int)mOutput->write(curBuf, mixBufferSize);
                if (bytesWritten > 0) mBytesWritten += bytesWritten;
                mNumWrites++;
                mInWrite = false;
                mStandby = false;
                nsecs_t temp = systemTime();
                standbyTime = temp + kStandbyTimeInNsecs;
                nsecs_t delta = temp - mLastWriteTime;
                if (delta > maxPeriod) {
                    LOGW("write blocked for %llu msecs", ns2ms(delta));
                    mNumDelayedWrites++;
                }
                sleepTime = kBufferRecoveryInUsecs;
            }
        } else {
            // There was nothing to mix this round, which means all
            // active tracks were late. Sleep a little bit to give
            // them another chance. If we're too late, the audio
            // hardware will zero-fill for us.
            // LOGV("thread %p no buffers - usleep(%lu)", this, sleepTime);
            usleep(sleepTime);
            if (sleepTime < kMaxBufferRecoveryInUsecs) {
                sleepTime += kBufferRecoveryInUsecs;
            }
        }

        // finally let go of all our tracks, without the lock held
        // since we can't guarantee the destructors won't acquire that
        // same lock.
        tracksToRemove.clear();
    }

    if (!mStandby) {
        mOutput->standby();
    }
    sendConfigEvent(AudioSystem::OUTPUT_CLOSED);
    processConfigEvents();

    LOGV("MixerThread %p exiting", this);
    return false;
}

// prepareTracks_l() must be called with ThreadBase::mLock held
size_t AudioFlinger::MixerThread::prepareTracks_l(const SortedVector< wp<Track> >& activeTracks, Vector< sp<Track> > *tracksToRemove)
{

    size_t enabledTracks = 0;
    // find out which tracks need to be processed
    size_t count = activeTracks.size();
    for (size_t i=0 ; i<count ; i++) {
        sp<Track> t = activeTracks[i].promote();
        if (t == 0) continue;

        Track* const track = t.get();
        audio_track_cblk_t* cblk = track->cblk();

        // The first time a track is added we wait
        // for all its buffers to be filled before processing it
        mAudioMixer->setActiveTrack(track->name());
        if (cblk->framesReady() && (track->isReady() || track->isStopped()) &&
                !track->isPaused())
        {
            //LOGV("track %d u=%08x, s=%08x [OK]", track->name(), cblk->user, cblk->server);

            // compute volume for this track
            int16_t left, right;
            if (track->isMuted() || mMasterMute || track->isPausing() ||
                mStreamTypes[track->type()].mute) {
                left = right = 0;
                if (track->isPausing()) {
                    track->setPaused();
                }
            } else {
                float typeVolume = mStreamTypes[track->type()].volume;
                float v = mMasterVolume * typeVolume;
                float v_clamped = v * cblk->volume[0];
                if (v_clamped > MAX_GAIN) v_clamped = MAX_GAIN;
                left = int16_t(v_clamped);
                v_clamped = v * cblk->volume[1];
                if (v_clamped > MAX_GAIN) v_clamped = MAX_GAIN;
                right = int16_t(v_clamped);
            }

            // XXX: these things DON'T need to be done each time
            mAudioMixer->setBufferProvider(track);
            mAudioMixer->enable(AudioMixer::MIXING);

            int param;
            if ( track->mFillingUpStatus == Track::FS_FILLED) {
                // no ramp for the first volume setting
                track->mFillingUpStatus = Track::FS_ACTIVE;
                if (track->mState == TrackBase::RESUMING) {
                    track->mState = TrackBase::ACTIVE;
                    param = AudioMixer::RAMP_VOLUME;
                } else {
                    param = AudioMixer::VOLUME;
                }
            } else {
                param = AudioMixer::RAMP_VOLUME;
            }
            mAudioMixer->setParameter(param, AudioMixer::VOLUME0, left);
            mAudioMixer->setParameter(param, AudioMixer::VOLUME1, right);
            mAudioMixer->setParameter(
                AudioMixer::TRACK,
                AudioMixer::FORMAT, track->format());
            mAudioMixer->setParameter(
                AudioMixer::TRACK,
                AudioMixer::CHANNEL_COUNT, track->channelCount());
            mAudioMixer->setParameter(
                AudioMixer::RESAMPLE,
                AudioMixer::SAMPLE_RATE,
                int(cblk->sampleRate));

            // reset retry count
            track->mRetryCount = kMaxTrackRetries;
            enabledTracks++;
        } else {
            //LOGV("track %d u=%08x, s=%08x [NOT READY]", track->name(), cblk->user, cblk->server);
            if (track->isStopped()) {
                track->reset();
            }
            if (track->isTerminated() || track->isStopped() || track->isPaused()) {
                // We have consumed all the buffers of this track.
                // Remove it from the list of active tracks.
                tracksToRemove->add(track);
                mAudioMixer->disable(AudioMixer::MIXING);
            } else {
                // No buffers for this track. Give it a few chances to
                // fill a buffer, then remove it from active list.
                if (--(track->mRetryCount) <= 0) {
                    LOGV("BUFFER TIMEOUT: remove(%d) from active list", track->name());
                    tracksToRemove->add(track);
                }
                // For tracks using static shared memry buffer, make sure that we have
                // written enough data to audio hardware before disabling the track
                // NOTE: this condition with arrive before track->mRetryCount <= 0 so we
                // don't care about code removing track from active list above.
                if ((track->mSharedBuffer == 0) || (mBytesWritten >= mMinBytesToWrite)) {
                    mAudioMixer->disable(AudioMixer::MIXING);
                } else {
                    enabledTracks++;
                }
            }
        }
    }

    // remove all the tracks that need to be...
    count = tracksToRemove->size();
    if (UNLIKELY(count)) {
        for (size_t i=0 ; i<count ; i++) {
            const sp<Track>& track = tracksToRemove->itemAt(i);
            mActiveTracks.remove(track);
            if (track->isTerminated()) {
                mTracks.remove(track);
                deleteTrackName_l(track->mName);
            }
        }
    }

    return enabledTracks;
}

void AudioFlinger::MixerThread::getTracks(
        SortedVector < sp<Track> >& tracks,
        SortedVector < wp<Track> >& activeTracks,
        int streamType)
{
    LOGV ("MixerThread::getTracks() mixer %p, mTracks.size %d, mActiveTracks.size %d", this,  mTracks.size(), mActiveTracks.size());
    Mutex::Autolock _l(mLock);
    size_t size = mTracks.size();
    for (size_t i = 0; i < size; i++) {
        sp<Track> t = mTracks[i];
        if (t->type() == streamType) {
            tracks.add(t);
            int j = mActiveTracks.indexOf(t);
            if (j >= 0) {
                t = mActiveTracks[j].promote();
                if (t != NULL) {
                    activeTracks.add(t);
                }
            }
        }
    }

    size = activeTracks.size();
    for (size_t i = 0; i < size; i++) {
        mActiveTracks.remove(activeTracks[i]);
    }

    size = tracks.size();
    for (size_t i = 0; i < size; i++) {
        sp<Track> t = tracks[i];
        mTracks.remove(t);
        deleteTrackName_l(t->name());
    }
}

void AudioFlinger::MixerThread::putTracks(
        SortedVector < sp<Track> >& tracks,
        SortedVector < wp<Track> >& activeTracks)
{
    LOGV ("MixerThread::putTracks() mixer %p, tracks.size %d, activeTracks.size %d", this,  tracks.size(), activeTracks.size());
    Mutex::Autolock _l(mLock);
    size_t size = tracks.size();
    for (size_t i = 0; i < size ; i++) {
        sp<Track> t = tracks[i];
        int name = getTrackName_l();

        if (name < 0) return;

        t->mName = name;
        t->mThread = this;
        mTracks.add(t);

        int j = activeTracks.indexOf(t);
        if (j >= 0) {
            mActiveTracks.add(t);
        }
    }
}

// getTrackName_l() must be called with ThreadBase::mLock held
int AudioFlinger::MixerThread::getTrackName_l()
{
    return mAudioMixer->getTrackName();
}

// deleteTrackName_l() must be called with ThreadBase::mLock held
void AudioFlinger::MixerThread::deleteTrackName_l(int name)
{
    mAudioMixer->deleteTrackName(name);
}

// checkForNewParameters_l() must be called with ThreadBase::mLock held
bool AudioFlinger::MixerThread::checkForNewParameters_l()
{
    bool reconfig = false;

    while (!mNewParameters.isEmpty()) {
        status_t status = NO_ERROR;
        String8 keyValuePair = mNewParameters[0];
        AudioParameter param = AudioParameter(keyValuePair);
        int value;

        mNewParameters.removeAt(0);

        if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
            reconfig = true;
        }
        if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
            if (value != AudioSystem::PCM_16_BIT) {
                status = BAD_VALUE;
            } else {
                reconfig = true;
            }
        }
        if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
            if (value != AudioSystem::CHANNEL_OUT_STEREO) {
                status = BAD_VALUE;
            } else {
                reconfig = true;
            }
        }
        if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
            // do not accept frame count changes if tracks are open as the track buffer
            // size depends on frame count and correct behavior would not be garantied
            // if frame count is changed after track creation
            if (!mTracks.isEmpty()) {
                status = INVALID_OPERATION;
            } else {
                reconfig = true;
            }
        }
        if (status == NO_ERROR) {
            status = mOutput->setParameters(keyValuePair);
            if (!mStandby && status == INVALID_OPERATION) {
               mOutput->standby();
               mStandby = true;
               mBytesWritten = 0;
               status = mOutput->setParameters(keyValuePair);
            }
            if (status == NO_ERROR && reconfig) {
                delete mAudioMixer;
                readOutputParameters();
                mAudioMixer = new AudioMixer(mFrameCount, mSampleRate);
                for (size_t i = 0; i < mTracks.size() ; i++) {
                    int name = getTrackName_l();
                    if (name < 0) break;
                    mTracks[i]->mName = name;
                }
                sendConfigEvent_l(AudioSystem::OUTPUT_CONFIG_CHANGED);
            }
        }
        mParamStatus = status;
        mParamCond.signal();
        mWaitWorkCV.wait(mLock);
    }
    return reconfig;
}

status_t AudioFlinger::MixerThread::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    PlaybackThread::dumpInternals(fd, args);

    snprintf(buffer, SIZE, "AudioMixer tracks: %08x\n", mAudioMixer->trackNames());
    result.append(buffer);
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

// ----------------------------------------------------------------------------
AudioFlinger::DirectOutputThread::DirectOutputThread(const sp<AudioFlinger>& audioFlinger, AudioStreamOut* output)
    :   PlaybackThread(audioFlinger, output),
    mLeftVolume (1.0), mRightVolume(1.0)
{
    mType = PlaybackThread::DIRECT;
}

AudioFlinger::DirectOutputThread::~DirectOutputThread()
{
}


bool AudioFlinger::DirectOutputThread::threadLoop()
{
    unsigned long sleepTime = kBufferRecoveryInUsecs;
    sp<Track> trackToRemove;
    sp<Track> activeTrack;
    nsecs_t standbyTime = systemTime();
    int8_t *curBuf;
    size_t mixBufferSize = mFrameCount*mFrameSize;

    while (!exitPending())
    {
        processConfigEvents();

        { // scope for the mLock

            Mutex::Autolock _l(mLock);

            if (checkForNewParameters_l()) {
                mixBufferSize = mFrameCount*mFrameSize;
            }

            // put audio hardware into standby after short delay
            if UNLIKELY((!mActiveTracks.size() && systemTime() > standbyTime) ||
                        mSuspended) {
                // wait until we have something to do...
                if (!mStandby) {
                    LOGV("Audio hardware entering standby, mixer %p\n", this);
                    mOutput->standby();
                    mStandby = true;
                    mBytesWritten = 0;
                }

                if (!mActiveTracks.size() && mConfigEvents.isEmpty()) {
                    // we're about to wait, flush the binder command buffer
                    IPCThreadState::self()->flushCommands();

                    if (exitPending()) break;

                    LOGV("DirectOutputThread %p TID %d going to sleep\n", this, gettid());
                    mWaitWorkCV.wait(mLock);
                    LOGV("DirectOutputThread %p TID %d waking up in active mode\n", this, gettid());

                    if (mMasterMute == false) {
                        char value[PROPERTY_VALUE_MAX];
                        property_get("ro.audio.silent", value, "0");
                        if (atoi(value)) {
                            LOGD("Silence is golden");
                            setMasterMute(true);
                        }
                    }

                    standbyTime = systemTime() + kStandbyTimeInNsecs;
                    continue;
                }
            }

            // find out which tracks need to be processed
            if (mActiveTracks.size() != 0) {
                sp<Track> t = mActiveTracks[0].promote();
                if (t == 0) continue;

                Track* const track = t.get();
                audio_track_cblk_t* cblk = track->cblk();

                // The first time a track is added we wait
                // for all its buffers to be filled before processing it
                if (cblk->framesReady() && (track->isReady() || track->isStopped()) &&
                        !track->isPaused())
                {
                    //LOGV("track %d u=%08x, s=%08x [OK]", track->name(), cblk->user, cblk->server);

                    // compute volume for this track
                    float left, right;
                    if (track->isMuted() || mMasterMute || track->isPausing() ||
                        mStreamTypes[track->type()].mute) {
                        left = right = 0;
                        if (track->isPausing()) {
                            track->setPaused();
                        }
                    } else {
                        float typeVolume = mStreamTypes[track->type()].volume;
                        float v = mMasterVolume * typeVolume;
                        float v_clamped = v * cblk->volume[0];
                        if (v_clamped > MAX_GAIN) v_clamped = MAX_GAIN;
                        left = v_clamped/MAX_GAIN;
                        v_clamped = v * cblk->volume[1];
                        if (v_clamped > MAX_GAIN) v_clamped = MAX_GAIN;
                        right = v_clamped/MAX_GAIN;
                    }

                    if (left != mLeftVolume || right != mRightVolume) {
                        mOutput->setVolume(left, right);
                        left = mLeftVolume;
                        right = mRightVolume;
                    }

                    if (track->mFillingUpStatus == Track::FS_FILLED) {
                        track->mFillingUpStatus = Track::FS_ACTIVE;
                        if (track->mState == TrackBase::RESUMING) {
                            track->mState = TrackBase::ACTIVE;
                        }
                    }

                    // reset retry count
                    track->mRetryCount = kMaxTrackRetries;
                    activeTrack = t;
                } else {
                    //LOGV("track %d u=%08x, s=%08x [NOT READY]", track->name(), cblk->user, cblk->server);
                    if (track->isStopped()) {
                        track->reset();
                    }
                    if (track->isTerminated() || track->isStopped() || track->isPaused()) {
                        // We have consumed all the buffers of this track.
                        // Remove it from the list of active tracks.
                        trackToRemove = track;
                    } else {
                        // No buffers for this track. Give it a few chances to
                        // fill a buffer, then remove it from active list.
                        if (--(track->mRetryCount) <= 0) {
                            LOGV("BUFFER TIMEOUT: remove(%d) from active list", track->name());
                            trackToRemove = track;
                        }

                        // For tracks using static shared memry buffer, make sure that we have
                        // written enough data to audio hardware before disabling the track
                        // NOTE: this condition with arrive before track->mRetryCount <= 0 so we
                        // don't care about code removing track from active list above.
                        if ((track->mSharedBuffer != 0) && (mBytesWritten < mMinBytesToWrite)) {
                            activeTrack = t;
                        }
                     }
                }
            }

            // remove all the tracks that need to be...
            if (UNLIKELY(trackToRemove != 0)) {
                mActiveTracks.remove(trackToRemove);
                if (trackToRemove->isTerminated()) {
                    mTracks.remove(trackToRemove);
                    deleteTrackName_l(trackToRemove->mName);
                }
            }
       }

        if (activeTrack != 0) {
            AudioBufferProvider::Buffer buffer;
            size_t frameCount = mFrameCount;
            curBuf = (int8_t *)mMixBuffer;
            // output audio to hardware
            mLastWriteTime = systemTime();
            mInWrite = true;
            while(frameCount) {
                buffer.frameCount = frameCount;
                activeTrack->getNextBuffer(&buffer);
                if (UNLIKELY(buffer.raw == 0)) {
                    memset(curBuf, 0, frameCount * mFrameSize);
                    break;
                }
                memcpy(curBuf, buffer.raw, buffer.frameCount * mFrameSize);
                frameCount -= buffer.frameCount;
                curBuf += buffer.frameCount * mFrameSize;
                activeTrack->releaseBuffer(&buffer);
            }
            if (mSuspended) {
                usleep(kMaxBufferRecoveryInUsecs);
            } else {
                int bytesWritten = (int)mOutput->write(mMixBuffer, mixBufferSize);
                if (bytesWritten) mBytesWritten += bytesWritten;
                mNumWrites++;
                mInWrite = false;
                mStandby = false;
                nsecs_t temp = systemTime();
                standbyTime = temp + kStandbyTimeInNsecs;
                sleepTime = kBufferRecoveryInUsecs;
            }
        } else {
            // There was nothing to mix this round, which means all
            // active tracks were late. Sleep a little bit to give
            // them another chance. If we're too late, the audio
            // hardware will zero-fill for us.
            //LOGV("no buffers - usleep(%lu)", sleepTime);
            usleep(sleepTime);
            if (sleepTime < kMaxBufferRecoveryInUsecs) {
                sleepTime += kBufferRecoveryInUsecs;
            }
        }

        // finally let go of removed track, without the lock held
        // since we can't guarantee the destructors won't acquire that
        // same lock.
        trackToRemove.clear();
        activeTrack.clear();
    }

    if (!mStandby) {
        mOutput->standby();
    }
    sendConfigEvent(AudioSystem::OUTPUT_CLOSED);
    processConfigEvents();

    LOGV("DirectOutputThread %p exiting", this);
    return false;
}

// getTrackName_l() must be called with ThreadBase::mLock held
int AudioFlinger::DirectOutputThread::getTrackName_l()
{
    return 0;
}

// deleteTrackName_l() must be called with ThreadBase::mLock held
void AudioFlinger::DirectOutputThread::deleteTrackName_l(int name)
{
}

// checkForNewParameters_l() must be called with ThreadBase::mLock held
bool AudioFlinger::DirectOutputThread::checkForNewParameters_l()
{
    bool reconfig = false;

    while (!mNewParameters.isEmpty()) {
        status_t status = NO_ERROR;
        String8 keyValuePair = mNewParameters[0];
        AudioParameter param = AudioParameter(keyValuePair);
        int value;

        mNewParameters.removeAt(0);

        if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
            // do not accept frame count changes if tracks are open as the track buffer
            // size depends on frame count and correct behavior would not be garantied
            // if frame count is changed after track creation
            if (!mTracks.isEmpty()) {
                status = INVALID_OPERATION;
            } else {
                reconfig = true;
            }
        }
        if (status == NO_ERROR) {
            status = mOutput->setParameters(keyValuePair);
            if (!mStandby && status == INVALID_OPERATION) {
               mOutput->standby();
               mStandby = true;
               mBytesWritten = 0;
               status = mOutput->setParameters(keyValuePair);
            }
            if (status == NO_ERROR && reconfig) {
                readOutputParameters();
                sendConfigEvent_l(AudioSystem::OUTPUT_CONFIG_CHANGED);
            }
        }
        mParamStatus = status;
        mParamCond.signal();
        mWaitWorkCV.wait(mLock);
    }
    return reconfig;
}

// ----------------------------------------------------------------------------

AudioFlinger::DuplicatingThread::DuplicatingThread(const sp<AudioFlinger>& audioFlinger, AudioFlinger::MixerThread* mainThread)
    :   MixerThread(audioFlinger, mainThread->getOutput())
{
    mType = PlaybackThread::DUPLICATING;
    addOutputTrack(mainThread);
}

AudioFlinger::DuplicatingThread::~DuplicatingThread()
{
    mOutputTracks.clear();
}

bool AudioFlinger::DuplicatingThread::threadLoop()
{
    unsigned long sleepTime = kBufferRecoveryInUsecs;
    int16_t* curBuf = mMixBuffer;
    Vector< sp<Track> > tracksToRemove;
    size_t enabledTracks = 0;
    nsecs_t standbyTime = systemTime();
    size_t mixBufferSize = mFrameCount*mFrameSize;
    SortedVector< sp<OutputTrack> > outputTracks;

    while (!exitPending())
    {
        processConfigEvents();

        enabledTracks = 0;
        { // scope for the mLock

            Mutex::Autolock _l(mLock);

            if (checkForNewParameters_l()) {
                mixBufferSize = mFrameCount*mFrameSize;
            }

            const SortedVector< wp<Track> >& activeTracks = mActiveTracks;

            for (size_t i = 0; i < mOutputTracks.size(); i++) {
                outputTracks.add(mOutputTracks[i]);
            }

            // put audio hardware into standby after short delay
            if UNLIKELY((!activeTracks.size() && systemTime() > standbyTime) ||
                         mSuspended) {
                if (!mStandby) {
                    for (size_t i = 0; i < outputTracks.size(); i++) {
                        mLock.unlock();
                        outputTracks[i]->stop();
                        mLock.lock();
                    }
                    mStandby = true;
                    mBytesWritten = 0;
                }

                if (!activeTracks.size() && mConfigEvents.isEmpty()) {
                    // we're about to wait, flush the binder command buffer
                    IPCThreadState::self()->flushCommands();
                    outputTracks.clear();

                    if (exitPending()) break;

                    LOGV("DuplicatingThread %p TID %d going to sleep\n", this, gettid());
                    mWaitWorkCV.wait(mLock);
                    LOGV("DuplicatingThread %p TID %d waking up\n", this, gettid());
                    if (mMasterMute == false) {
                        char value[PROPERTY_VALUE_MAX];
                        property_get("ro.audio.silent", value, "0");
                        if (atoi(value)) {
                            LOGD("Silence is golden");
                            setMasterMute(true);
                        }
                    }

                    standbyTime = systemTime() + kStandbyTimeInNsecs;
                    sleepTime = kBufferRecoveryInUsecs;
                    continue;
                }
            }

            enabledTracks = prepareTracks_l(activeTracks, &tracksToRemove);
       }

        bool mustSleep = true;
        if (LIKELY(enabledTracks)) {
            // mix buffers...
            mAudioMixer->process(curBuf);
            if (!mSuspended) {
                for (size_t i = 0; i < outputTracks.size(); i++) {
                    outputTracks[i]->write(curBuf, mFrameCount);
                }
                mStandby = false;
                mustSleep = false;
                mBytesWritten += mixBufferSize;
            }
        } else {
            // flush remaining overflow buffers in output tracks
            for (size_t i = 0; i < outputTracks.size(); i++) {
                if (outputTracks[i]->isActive()) {
                    outputTracks[i]->write(curBuf, 0);
                    standbyTime = systemTime() + kStandbyTimeInNsecs;
                    mustSleep = false;
                }
            }
        }
        if (mustSleep) {
//            LOGV("threadLoop() sleeping %d", sleepTime);
            usleep(sleepTime);
            if (sleepTime < kMaxBufferRecoveryInUsecs) {
                sleepTime += kBufferRecoveryInUsecs;
            }
        } else {
            sleepTime = kBufferRecoveryInUsecs;
        }

        // finally let go of all our tracks, without the lock held
        // since we can't guarantee the destructors won't acquire that
        // same lock.
        tracksToRemove.clear();
        outputTracks.clear();
    }

    if (!mStandby) {
        for (size_t i = 0; i < outputTracks.size(); i++) {
            mLock.unlock();
            outputTracks[i]->stop();
            mLock.lock();
        }
    }

    sendConfigEvent(AudioSystem::OUTPUT_CLOSED);
    processConfigEvents();

    return false;
}

void AudioFlinger::DuplicatingThread::addOutputTrack(MixerThread *thread)
{
    int frameCount = (3 * mFrameCount * mSampleRate) / thread->sampleRate();
    OutputTrack *outputTrack = new OutputTrack((ThreadBase *)thread,
                                            mSampleRate,
                                            mFormat,
                                            mChannelCount,
                                            frameCount);
    thread->setStreamVolume(AudioSystem::NUM_STREAM_TYPES, 1.0f);
    mOutputTracks.add(outputTrack);
    LOGV("addOutputTrack() track %p, on thread %p", outputTrack, thread);
}

void AudioFlinger::DuplicatingThread::removeOutputTrack(MixerThread *thread)
{
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        if (mOutputTracks[i]->thread() == (ThreadBase *)thread) {
            mOutputTracks.removeAt(i);
            return;
        }
    }
    LOGV("removeOutputTrack(): unkonwn thread: %p", thread);
}


// ----------------------------------------------------------------------------

// TrackBase constructor must be called with AudioFlinger::mLock held
AudioFlinger::ThreadBase::TrackBase::TrackBase(
            const wp<ThreadBase>& thread,
            const sp<Client>& client,
            uint32_t sampleRate,
            int format,
            int channelCount,
            int frameCount,
            uint32_t flags,
            const sp<IMemory>& sharedBuffer)
    :   RefBase(),
        mThread(thread),
        mClient(client),
        mFrameCount(0),
        mState(IDLE),
        mClientTid(-1),
        mFormat(format),
        mFlags(flags & ~SYSTEM_FLAGS_MASK)
{
    LOGV_IF(sharedBuffer != 0, "sharedBuffer: %p, size: %d", sharedBuffer->pointer(), sharedBuffer->size());

    // LOGD("Creating track with %d buffers @ %d bytes", bufferCount, bufferSize);
   size_t size = sizeof(audio_track_cblk_t);
   size_t bufferSize = frameCount*channelCount*sizeof(int16_t);
   if (sharedBuffer == 0) {
       size += bufferSize;
   }

   if (client != NULL) {
        mCblkMemory = client->heap()->allocate(size);
        if (mCblkMemory != 0) {
            mCblk = static_cast<audio_track_cblk_t *>(mCblkMemory->pointer());
            if (mCblk) { // construct the shared structure in-place.
                new(mCblk) audio_track_cblk_t();
                // clear all buffers
                mCblk->frameCount = frameCount;
                mCblk->sampleRate = sampleRate;
                mCblk->channels = (uint8_t)channelCount;
                if (sharedBuffer == 0) {
                    mBuffer = (char*)mCblk + sizeof(audio_track_cblk_t);
                    memset(mBuffer, 0, frameCount*channelCount*sizeof(int16_t));
                    // Force underrun condition to avoid false underrun callback until first data is
                    // written to buffer
                    mCblk->flowControlFlag = 1;
                } else {
                    mBuffer = sharedBuffer->pointer();
                }
                mBufferEnd = (uint8_t *)mBuffer + bufferSize;
            }
        } else {
            LOGE("not enough memory for AudioTrack size=%u", size);
            client->heap()->dump("AudioTrack");
            return;
        }
   } else {
       mCblk = (audio_track_cblk_t *)(new uint8_t[size]);
       if (mCblk) { // construct the shared structure in-place.
           new(mCblk) audio_track_cblk_t();
           // clear all buffers
           mCblk->frameCount = frameCount;
           mCblk->sampleRate = sampleRate;
           mCblk->channels = (uint8_t)channelCount;
           mBuffer = (char*)mCblk + sizeof(audio_track_cblk_t);
           memset(mBuffer, 0, frameCount*channelCount*sizeof(int16_t));
           // Force underrun condition to avoid false underrun callback until first data is
           // written to buffer
           mCblk->flowControlFlag = 1;
           mBufferEnd = (uint8_t *)mBuffer + bufferSize;
       }
   }
}

AudioFlinger::PlaybackThread::TrackBase::~TrackBase()
{
    if (mCblk) {
        mCblk->~audio_track_cblk_t();   // destroy our shared-structure.
        if (mClient == NULL) {
            delete mCblk;
        }
    }
    mCblkMemory.clear();            // and free the shared memory
    mClient.clear();
}

void AudioFlinger::PlaybackThread::TrackBase::releaseBuffer(AudioBufferProvider::Buffer* buffer)
{
    buffer->raw = 0;
    mFrameCount = buffer->frameCount;
    step();
    buffer->frameCount = 0;
}

bool AudioFlinger::PlaybackThread::TrackBase::step() {
    bool result;
    audio_track_cblk_t* cblk = this->cblk();

    result = cblk->stepServer(mFrameCount);
    if (!result) {
        LOGV("stepServer failed acquiring cblk mutex");
        mFlags |= STEPSERVER_FAILED;
    }
    return result;
}

void AudioFlinger::PlaybackThread::TrackBase::reset() {
    audio_track_cblk_t* cblk = this->cblk();

    cblk->user = 0;
    cblk->server = 0;
    cblk->userBase = 0;
    cblk->serverBase = 0;
    mFlags &= (uint32_t)(~SYSTEM_FLAGS_MASK);
    LOGV("TrackBase::reset");
}

sp<IMemory> AudioFlinger::PlaybackThread::TrackBase::getCblk() const
{
    return mCblkMemory;
}

int AudioFlinger::PlaybackThread::TrackBase::sampleRate() const {
    return (int)mCblk->sampleRate;
}

int AudioFlinger::PlaybackThread::TrackBase::channelCount() const {
    return (int)mCblk->channels;
}

void* AudioFlinger::PlaybackThread::TrackBase::getBuffer(uint32_t offset, uint32_t frames) const {
    audio_track_cblk_t* cblk = this->cblk();
    int8_t *bufferStart = (int8_t *)mBuffer + (offset-cblk->serverBase)*cblk->frameSize;
    int8_t *bufferEnd = bufferStart + frames * cblk->frameSize;

    // Check validity of returned pointer in case the track control block would have been corrupted.
    if (bufferStart < mBuffer || bufferStart > bufferEnd || bufferEnd > mBufferEnd ||
        ((unsigned long)bufferStart & (unsigned long)(cblk->frameSize - 1))) {
        LOGE("TrackBase::getBuffer buffer out of range:\n    start: %p, end %p , mBuffer %p mBufferEnd %p\n    \
                server %d, serverBase %d, user %d, userBase %d, channels %d",
                bufferStart, bufferEnd, mBuffer, mBufferEnd,
                cblk->server, cblk->serverBase, cblk->user, cblk->userBase, cblk->channels);
        return 0;
    }

    return bufferStart;
}

// ----------------------------------------------------------------------------

// Track constructor must be called with AudioFlinger::mLock and ThreadBase::mLock held
AudioFlinger::PlaybackThread::Track::Track(
            const wp<ThreadBase>& thread,
            const sp<Client>& client,
            int streamType,
            uint32_t sampleRate,
            int format,
            int channelCount,
            int frameCount,
            const sp<IMemory>& sharedBuffer)
    :   TrackBase(thread, client, sampleRate, format, channelCount, frameCount, 0, sharedBuffer),
    mMute(false), mSharedBuffer(sharedBuffer), mName(-1)
{
    sp<ThreadBase> baseThread = thread.promote();
    if (baseThread != 0) {
        PlaybackThread *playbackThread = (PlaybackThread *)baseThread.get();
        mName = playbackThread->getTrackName_l();
    }
    LOGV("Track constructor name %d, calling thread %d", mName, IPCThreadState::self()->getCallingPid());
    if (mName < 0) {
        LOGE("no more track names available");
    }
    mVolume[0] = 1.0f;
    mVolume[1] = 1.0f;
    mStreamType = streamType;
    // NOTE: audio_track_cblk_t::frameSize for 8 bit PCM data is based on a sample size of
    // 16 bit because data is converted to 16 bit before being stored in buffer by AudioTrack
    mCblk->frameSize = AudioSystem::isLinearPCM(format) ? channelCount * sizeof(int16_t) : sizeof(int8_t);
}

AudioFlinger::PlaybackThread::Track::~Track()
{
    LOGV("PlaybackThread::Track destructor");
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        mState = TERMINATED;
    }
}

void AudioFlinger::PlaybackThread::Track::destroy()
{
    // NOTE: destroyTrack_l() can remove a strong reference to this Track
    // by removing it from mTracks vector, so there is a risk that this Tracks's
    // desctructor is called. As the destructor needs to lock mLock,
    // we must acquire a strong reference on this Track before locking mLock
    // here so that the destructor is called only when exiting this function.
    // On the other hand, as long as Track::destroy() is only called by
    // TrackHandle destructor, the TrackHandle still holds a strong ref on
    // this Track with its member mTrack.
    sp<Track> keep(this);
    { // scope for mLock
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            Mutex::Autolock _l(thread->mLock);
            PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
            playbackThread->destroyTrack_l(this);
        }
    }
}

void AudioFlinger::PlaybackThread::Track::dump(char* buffer, size_t size)
{
    snprintf(buffer, size, "  %5d %5d %3u %3u %3u %3u %1d %1d %1d %5u %5u %5u %04x %04x\n",
            mName - AudioMixer::TRACK0,
            (mClient == NULL) ? getpid() : mClient->pid(),
            mStreamType,
            mFormat,
            mCblk->channels,
            mFrameCount,
            mState,
            mMute,
            mFillingUpStatus,
            mCblk->sampleRate,
            mCblk->volume[0],
            mCblk->volume[1],
            mCblk->server,
            mCblk->user);
}

status_t AudioFlinger::PlaybackThread::Track::getNextBuffer(AudioBufferProvider::Buffer* buffer)
{
     audio_track_cblk_t* cblk = this->cblk();
     uint32_t framesReady;
     uint32_t framesReq = buffer->frameCount;

     // Check if last stepServer failed, try to step now
     if (mFlags & TrackBase::STEPSERVER_FAILED) {
         if (!step())  goto getNextBuffer_exit;
         LOGV("stepServer recovered");
         mFlags &= ~TrackBase::STEPSERVER_FAILED;
     }

     framesReady = cblk->framesReady();

     if (LIKELY(framesReady)) {
        uint32_t s = cblk->server;
        uint32_t bufferEnd = cblk->serverBase + cblk->frameCount;

        bufferEnd = (cblk->loopEnd < bufferEnd) ? cblk->loopEnd : bufferEnd;
        if (framesReq > framesReady) {
            framesReq = framesReady;
        }
        if (s + framesReq > bufferEnd) {
            framesReq = bufferEnd - s;
        }

         buffer->raw = getBuffer(s, framesReq);
         if (buffer->raw == 0) goto getNextBuffer_exit;

         buffer->frameCount = framesReq;
        return NO_ERROR;
     }

getNextBuffer_exit:
     buffer->raw = 0;
     buffer->frameCount = 0;
     LOGV("getNextBuffer() no more data");
     return NOT_ENOUGH_DATA;
}

bool AudioFlinger::PlaybackThread::Track::isReady() const {
    if (mFillingUpStatus != FS_FILLING) return true;

    if (mCblk->framesReady() >= mCblk->frameCount ||
        mCblk->forceReady) {
        mFillingUpStatus = FS_FILLED;
        mCblk->forceReady = 0;
        return true;
    }
    return false;
}

status_t AudioFlinger::PlaybackThread::Track::start()
{
    LOGV("start(%d), calling thread %d", mName, IPCThreadState::self()->getCallingPid());
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
        playbackThread->addTrack_l(this);
    }
    return NO_ERROR;
}

void AudioFlinger::PlaybackThread::Track::stop()
{
    LOGV("stop(%d), calling thread %d", mName, IPCThreadState::self()->getCallingPid());
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        if (mState > STOPPED) {
            mState = STOPPED;
            // If the track is not active (PAUSED and buffers full), flush buffers
            PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
            if (playbackThread->mActiveTracks.indexOf(this) < 0) {
                reset();
            }
            LOGV("(> STOPPED) => STOPPED (%d)", mName);
        }
    }
}

void AudioFlinger::PlaybackThread::Track::pause()
{
    LOGV("pause(%d), calling thread %d", mName, IPCThreadState::self()->getCallingPid());
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        if (mState == ACTIVE || mState == RESUMING) {
            mState = PAUSING;
            LOGV("ACTIVE/RESUMING => PAUSING (%d)", mName);
        }
    }
}

void AudioFlinger::PlaybackThread::Track::flush()
{
    LOGV("flush(%d)", mName);
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        if (mState != STOPPED && mState != PAUSED && mState != PAUSING) {
            return;
        }
        // No point remaining in PAUSED state after a flush => go to
        // STOPPED state
        mState = STOPPED;

        mCblk->lock.lock();
        // NOTE: reset() will reset cblk->user and cblk->server with
        // the risk that at the same time, the AudioMixer is trying to read
        // data. In this case, getNextBuffer() would return a NULL pointer
        // as audio buffer => the AudioMixer code MUST always test that pointer
        // returned by getNextBuffer() is not NULL!
        reset();
        mCblk->lock.unlock();
    }
}

void AudioFlinger::PlaybackThread::Track::reset()
{
    // Do not reset twice to avoid discarding data written just after a flush and before
    // the audioflinger thread detects the track is stopped.
    if (!mResetDone) {
        TrackBase::reset();
        // Force underrun condition to avoid false underrun callback until first data is
        // written to buffer
        mCblk->flowControlFlag = 1;
        mCblk->forceReady = 0;
        mFillingUpStatus = FS_FILLING;
        mResetDone = true;
    }
}

void AudioFlinger::PlaybackThread::Track::mute(bool muted)
{
    mMute = muted;
}

void AudioFlinger::PlaybackThread::Track::setVolume(float left, float right)
{
    mVolume[0] = left;
    mVolume[1] = right;
}

// ----------------------------------------------------------------------------

// RecordTrack constructor must be called with AudioFlinger::mLock held
AudioFlinger::RecordThread::RecordTrack::RecordTrack(
            const wp<ThreadBase>& thread,
            const sp<Client>& client,
            uint32_t sampleRate,
            int format,
            int channelCount,
            int frameCount,
            uint32_t flags)
    :   TrackBase(thread, client, sampleRate, format,
                  channelCount, frameCount, flags, 0),
        mOverflow(false)
{
   LOGV("RecordTrack constructor, size %d", (int)mBufferEnd - (int)mBuffer);
   if (format == AudioSystem::PCM_16_BIT) {
       mCblk->frameSize = channelCount * sizeof(int16_t);
   } else if (format == AudioSystem::PCM_8_BIT) {
       mCblk->frameSize = channelCount * sizeof(int8_t);
   } else {
       mCblk->frameSize = sizeof(int8_t);
   }
}

AudioFlinger::RecordThread::RecordTrack::~RecordTrack()
{
}

status_t AudioFlinger::RecordThread::RecordTrack::getNextBuffer(AudioBufferProvider::Buffer* buffer)
{
    audio_track_cblk_t* cblk = this->cblk();
    uint32_t framesAvail;
    uint32_t framesReq = buffer->frameCount;

     // Check if last stepServer failed, try to step now
    if (mFlags & TrackBase::STEPSERVER_FAILED) {
        if (!step()) goto getNextBuffer_exit;
        LOGV("stepServer recovered");
        mFlags &= ~TrackBase::STEPSERVER_FAILED;
    }

    framesAvail = cblk->framesAvailable_l();

    if (LIKELY(framesAvail)) {
        uint32_t s = cblk->server;
        uint32_t bufferEnd = cblk->serverBase + cblk->frameCount;

        if (framesReq > framesAvail) {
            framesReq = framesAvail;
        }
        if (s + framesReq > bufferEnd) {
            framesReq = bufferEnd - s;
        }

        buffer->raw = getBuffer(s, framesReq);
        if (buffer->raw == 0) goto getNextBuffer_exit;

        buffer->frameCount = framesReq;
        return NO_ERROR;
    }

getNextBuffer_exit:
    buffer->raw = 0;
    buffer->frameCount = 0;
    return NOT_ENOUGH_DATA;
}

status_t AudioFlinger::RecordThread::RecordTrack::start()
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        RecordThread *recordThread = (RecordThread *)thread.get();
        return recordThread->start(this);
    }
    return NO_INIT;
}

void AudioFlinger::RecordThread::RecordTrack::stop()
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        RecordThread *recordThread = (RecordThread *)thread.get();
        recordThread->stop(this);
        TrackBase::reset();
        // Force overerrun condition to avoid false overrun callback until first data is
        // read from buffer
        mCblk->flowControlFlag = 1;
    }
}


// ----------------------------------------------------------------------------

AudioFlinger::PlaybackThread::OutputTrack::OutputTrack(
            const wp<ThreadBase>& thread,
            uint32_t sampleRate,
            int format,
            int channelCount,
            int frameCount)
    :   Track(thread, NULL, AudioSystem::NUM_STREAM_TYPES, sampleRate, format, channelCount, frameCount, NULL),
    mActive(false)
{

    PlaybackThread *playbackThread = (PlaybackThread *)thread.unsafe_get();
    mCblk->out = 1;
    mCblk->buffers = (char*)mCblk + sizeof(audio_track_cblk_t);
    mCblk->volume[0] = mCblk->volume[1] = 0x1000;
    mOutBuffer.frameCount = 0;
    mWaitTimeMs = (playbackThread->frameCount() * 2 * 1000) / playbackThread->sampleRate();

    LOGV("OutputTrack constructor mCblk %p, mBuffer %p, mCblk->buffers %p, mCblk->frameCount %d, mCblk->sampleRate %d, mCblk->channels %d mBufferEnd %p mWaitTimeMs %d",
            mCblk, mBuffer, mCblk->buffers, mCblk->frameCount, mCblk->sampleRate, mCblk->channels, mBufferEnd, mWaitTimeMs);

}

AudioFlinger::PlaybackThread::OutputTrack::~OutputTrack()
{
    stop();
}

status_t AudioFlinger::PlaybackThread::OutputTrack::start()
{
    status_t status = Track::start();
    if (status != NO_ERROR) {
        return status;
    }

    mActive = true;
    mRetryCount = 127;
    return status;
}

void AudioFlinger::PlaybackThread::OutputTrack::stop()
{
    Track::stop();
    clearBufferQueue();
    mOutBuffer.frameCount = 0;
    mActive = false;
}

bool AudioFlinger::PlaybackThread::OutputTrack::write(int16_t* data, uint32_t frames)
{
    Buffer *pInBuffer;
    Buffer inBuffer;
    uint32_t channels = mCblk->channels;
    bool outputBufferFull = false;
    inBuffer.frameCount = frames;
    inBuffer.i16 = data;

    uint32_t waitTimeLeftMs = mWaitTimeMs;

    if (!mActive) {
        start();
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            MixerThread *mixerThread = (MixerThread *)thread.get();
            if (mCblk->frameCount > frames){
                if (mBufferQueue.size() < kMaxOverFlowBuffers) {
                    uint32_t startFrames = (mCblk->frameCount - frames);
                    pInBuffer = new Buffer;
                    pInBuffer->mBuffer = new int16_t[startFrames * channels];
                    pInBuffer->frameCount = startFrames;
                    pInBuffer->i16 = pInBuffer->mBuffer;
                    memset(pInBuffer->raw, 0, startFrames * channels * sizeof(int16_t));
                    mBufferQueue.add(pInBuffer);
                } else {
                    LOGW ("OutputTrack::write() %p no more buffers in queue", this);
                }
            }
        }
    }

    while (waitTimeLeftMs) {
        // First write pending buffers, then new data
        if (mBufferQueue.size()) {
            pInBuffer = mBufferQueue.itemAt(0);
        } else {
            pInBuffer = &inBuffer;
        }

        if (pInBuffer->frameCount == 0) {
            break;
        }

        if (mOutBuffer.frameCount == 0) {
            mOutBuffer.frameCount = pInBuffer->frameCount;
            nsecs_t startTime = systemTime();
            if (obtainBuffer(&mOutBuffer, waitTimeLeftMs) == (status_t)AudioTrack::NO_MORE_BUFFERS) {
                LOGV ("OutputTrack::write() %p no more output buffers", this);
                outputBufferFull = true;
                break;
            }
            uint32_t waitTimeMs = (uint32_t)ns2ms(systemTime() - startTime);
//            LOGV("OutputTrack::write() waitTimeMs %d waitTimeLeftMs %d", waitTimeMs, waitTimeLeftMs)
            if (waitTimeLeftMs >= waitTimeMs) {
                waitTimeLeftMs -= waitTimeMs;
            } else {
                waitTimeLeftMs = 0;
            }
        }

        uint32_t outFrames = pInBuffer->frameCount > mOutBuffer.frameCount ? mOutBuffer.frameCount : pInBuffer->frameCount;
        memcpy(mOutBuffer.raw, pInBuffer->raw, outFrames * channels * sizeof(int16_t));
        mCblk->stepUser(outFrames);
        pInBuffer->frameCount -= outFrames;
        pInBuffer->i16 += outFrames * channels;
        mOutBuffer.frameCount -= outFrames;
        mOutBuffer.i16 += outFrames * channels;

        if (pInBuffer->frameCount == 0) {
            if (mBufferQueue.size()) {
                mBufferQueue.removeAt(0);
                delete [] pInBuffer->mBuffer;
                delete pInBuffer;
                LOGV("OutputTrack::write() %p released overflow buffer %d", this, mBufferQueue.size());
            } else {
                break;
            }
        }
    }

    // If we could not write all frames, allocate a buffer and queue it for next time.
    if (inBuffer.frameCount) {
        if (mBufferQueue.size() < kMaxOverFlowBuffers) {
            pInBuffer = new Buffer;
            pInBuffer->mBuffer = new int16_t[inBuffer.frameCount * channels];
            pInBuffer->frameCount = inBuffer.frameCount;
            pInBuffer->i16 = pInBuffer->mBuffer;
            memcpy(pInBuffer->raw, inBuffer.raw, inBuffer.frameCount * channels * sizeof(int16_t));
            mBufferQueue.add(pInBuffer);
            LOGV("OutputTrack::write() %p adding overflow buffer %d", this, mBufferQueue.size());
        } else {
            LOGW("OutputTrack::write() %p no more overflow buffers", this);
        }
    }

    // Calling write() with a 0 length buffer, means that no more data will be written:
    // If no more buffers are pending, fill output track buffer to make sure it is started
    // by output mixer.
    if (frames == 0 && mBufferQueue.size() == 0) {
        if (mCblk->user < mCblk->frameCount) {
            frames = mCblk->frameCount - mCblk->user;
            pInBuffer = new Buffer;
            pInBuffer->mBuffer = new int16_t[frames * channels];
            pInBuffer->frameCount = frames;
            pInBuffer->i16 = pInBuffer->mBuffer;
            memset(pInBuffer->raw, 0, frames * channels * sizeof(int16_t));
            mBufferQueue.add(pInBuffer);
        } else {
            stop();
        }
    }

    return outputBufferFull;
}

status_t AudioFlinger::PlaybackThread::OutputTrack::obtainBuffer(AudioBufferProvider::Buffer* buffer, uint32_t waitTimeMs)
{
    int active;
    status_t result;
    audio_track_cblk_t* cblk = mCblk;
    uint32_t framesReq = buffer->frameCount;

//    LOGV("OutputTrack::obtainBuffer user %d, server %d", cblk->user, cblk->server);
    buffer->frameCount  = 0;

    uint32_t framesAvail = cblk->framesAvailable();


    if (framesAvail == 0) {
        Mutex::Autolock _l(cblk->lock);
        goto start_loop_here;
        while (framesAvail == 0) {
            active = mActive;
            if (UNLIKELY(!active)) {
                LOGV("Not active and NO_MORE_BUFFERS");
                return AudioTrack::NO_MORE_BUFFERS;
            }
            result = cblk->cv.waitRelative(cblk->lock, milliseconds(waitTimeMs));
            if (result != NO_ERROR) {
                return AudioTrack::NO_MORE_BUFFERS;
            }
            // read the server count again
        start_loop_here:
            framesAvail = cblk->framesAvailable_l();
        }
    }

//    if (framesAvail < framesReq) {
//        return AudioTrack::NO_MORE_BUFFERS;
//    }

    if (framesReq > framesAvail) {
        framesReq = framesAvail;
    }

    uint32_t u = cblk->user;
    uint32_t bufferEnd = cblk->userBase + cblk->frameCount;

    if (u + framesReq > bufferEnd) {
        framesReq = bufferEnd - u;
    }

    buffer->frameCount  = framesReq;
    buffer->raw         = (void *)cblk->buffer(u);
    return NO_ERROR;
}


void AudioFlinger::PlaybackThread::OutputTrack::clearBufferQueue()
{
    size_t size = mBufferQueue.size();
    Buffer *pBuffer;

    for (size_t i = 0; i < size; i++) {
        pBuffer = mBufferQueue.itemAt(i);
        delete [] pBuffer->mBuffer;
        delete pBuffer;
    }
    mBufferQueue.clear();
}

// ----------------------------------------------------------------------------

AudioFlinger::Client::Client(const sp<AudioFlinger>& audioFlinger, pid_t pid)
    :   RefBase(),
        mAudioFlinger(audioFlinger),
        mMemoryDealer(new MemoryDealer(1024*1024)),
        mPid(pid)
{
    // 1 MB of address space is good for 32 tracks, 8 buffers each, 4 KB/buffer
}

AudioFlinger::Client::~Client()
{
    mAudioFlinger->removeClient(mPid);
}

const sp<MemoryDealer>& AudioFlinger::Client::heap() const
{
    return mMemoryDealer;
}

// ----------------------------------------------------------------------------

AudioFlinger::TrackHandle::TrackHandle(const sp<AudioFlinger::PlaybackThread::Track>& track)
    : BnAudioTrack(),
      mTrack(track)
{
}

AudioFlinger::TrackHandle::~TrackHandle() {
    // just stop the track on deletion, associated resources
    // will be freed from the main thread once all pending buffers have
    // been played. Unless it's not in the active track list, in which
    // case we free everything now...
    mTrack->destroy();
}

status_t AudioFlinger::TrackHandle::start() {
    return mTrack->start();
}

void AudioFlinger::TrackHandle::stop() {
    mTrack->stop();
}

void AudioFlinger::TrackHandle::flush() {
    mTrack->flush();
}

void AudioFlinger::TrackHandle::mute(bool e) {
    mTrack->mute(e);
}

void AudioFlinger::TrackHandle::pause() {
    mTrack->pause();
}

void AudioFlinger::TrackHandle::setVolume(float left, float right) {
    mTrack->setVolume(left, right);
}

sp<IMemory> AudioFlinger::TrackHandle::getCblk() const {
    return mTrack->getCblk();
}

status_t AudioFlinger::TrackHandle::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnAudioTrack::onTransact(code, data, reply, flags);
}

// ----------------------------------------------------------------------------

sp<IAudioRecord> AudioFlinger::openRecord(
        pid_t pid,
        int input,
        uint32_t sampleRate,
        int format,
        int channelCount,
        int frameCount,
        uint32_t flags,
        status_t *status)
{
    sp<RecordThread::RecordTrack> recordTrack;
    sp<RecordHandle> recordHandle;
    sp<Client> client;
    wp<Client> wclient;
    status_t lStatus;
    RecordThread *thread;
    size_t inFrameCount;

    // check calling permissions
    if (!recordingAllowed()) {
        lStatus = PERMISSION_DENIED;
        goto Exit;
    }

    // add client to list
    { // scope for mLock
        Mutex::Autolock _l(mLock);
        thread = checkRecordThread_l(input);
        if (thread == NULL) {
            lStatus = BAD_VALUE;
            goto Exit;
        }

        wclient = mClients.valueFor(pid);
        if (wclient != NULL) {
            client = wclient.promote();
        } else {
            client = new Client(this, pid);
            mClients.add(pid, client);
        }

        // create new record track. The record track uses one track in mHardwareMixerThread by convention.
        recordTrack = new RecordThread::RecordTrack(thread, client, sampleRate,
                                                   format, channelCount, frameCount, flags);
    }
    if (recordTrack->getCblk() == NULL) {
        recordTrack.clear();
        lStatus = NO_MEMORY;
        goto Exit;
    }

    // return to handle to client
    recordHandle = new RecordHandle(recordTrack);
    lStatus = NO_ERROR;

Exit:
    if (status) {
        *status = lStatus;
    }
    return recordHandle;
}

// ----------------------------------------------------------------------------

AudioFlinger::RecordHandle::RecordHandle(const sp<AudioFlinger::RecordThread::RecordTrack>& recordTrack)
    : BnAudioRecord(),
    mRecordTrack(recordTrack)
{
}

AudioFlinger::RecordHandle::~RecordHandle() {
    stop();
}

status_t AudioFlinger::RecordHandle::start() {
    LOGV("RecordHandle::start()");
    return mRecordTrack->start();
}

void AudioFlinger::RecordHandle::stop() {
    LOGV("RecordHandle::stop()");
    mRecordTrack->stop();
}

sp<IMemory> AudioFlinger::RecordHandle::getCblk() const {
    return mRecordTrack->getCblk();
}

status_t AudioFlinger::RecordHandle::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnAudioRecord::onTransact(code, data, reply, flags);
}

// ----------------------------------------------------------------------------

AudioFlinger::RecordThread::RecordThread(const sp<AudioFlinger>& audioFlinger, AudioStreamIn *input, uint32_t sampleRate, uint32_t channels) :
    ThreadBase(audioFlinger),
    mInput(input), mResampler(0), mRsmpOutBuffer(0), mRsmpInBuffer(0)
{
    mReqChannelCount = AudioSystem::popCount(channels);
    mReqSampleRate = sampleRate;
    readInputParameters();
    sendConfigEvent(AudioSystem::INPUT_OPENED);
}


AudioFlinger::RecordThread::~RecordThread()
{
    mAudioFlinger->mAudioHardware->closeInputStream(mInput);
    delete[] mRsmpInBuffer;
    if (mResampler != 0) {
        delete mResampler;
        delete[] mRsmpOutBuffer;
    }
}

void AudioFlinger::RecordThread::onFirstRef()
{
    const size_t SIZE = 256;
    char buffer[SIZE];

    snprintf(buffer, SIZE, "Record Thread %p", this);

    run(buffer, PRIORITY_URGENT_AUDIO);
}
bool AudioFlinger::RecordThread::threadLoop()
{
    AudioBufferProvider::Buffer buffer;
    sp<RecordTrack> activeTrack;

    // start recording
    while (!exitPending()) {

        processConfigEvents();

        { // scope for mLock
            Mutex::Autolock _l(mLock);
            checkForNewParameters_l();
            if (mActiveTrack == 0 && mConfigEvents.isEmpty()) {
                if (!mStandby) {
                    mInput->standby();
                    mStandby = true;
                }

                if (exitPending()) break;

                LOGV("RecordThread: loop stopping");
                // go to sleep
                mWaitWorkCV.wait(mLock);
                LOGV("RecordThread: loop starting");
                continue;
            }
            if (mActiveTrack != 0) {
                if (mActiveTrack->mState == TrackBase::PAUSING) {
                    mActiveTrack.clear();
                    mStartStopCond.broadcast();
                } else if (mActiveTrack->mState == TrackBase::RESUMING) {
                    mRsmpInIndex = mFrameCount;
                    if (mReqChannelCount != mActiveTrack->channelCount()) {
                        mActiveTrack.clear();
                    } else {
                        mActiveTrack->mState == TrackBase::ACTIVE;
                    }
                    mStartStopCond.broadcast();
                }
                mStandby = false;
            }
        }

        if (mActiveTrack != 0) {
            buffer.frameCount = mFrameCount;
            if (LIKELY(mActiveTrack->getNextBuffer(&buffer) == NO_ERROR)) {
                size_t framesOut = buffer.frameCount;
                if (mResampler == 0) {
                    // no resampling
                    while (framesOut) {
                        size_t framesIn = mFrameCount - mRsmpInIndex;
                        if (framesIn) {
                            int8_t *src = (int8_t *)mRsmpInBuffer + mRsmpInIndex * mFrameSize;
                            int8_t *dst = buffer.i8 + (buffer.frameCount - framesOut) * mActiveTrack->mCblk->frameSize;
                            if (framesIn > framesOut)
                                framesIn = framesOut;
                            mRsmpInIndex += framesIn;
                            framesOut -= framesIn;
                            if (mChannelCount == mReqChannelCount ||
                                mFormat != AudioSystem::PCM_16_BIT) {
                                memcpy(dst, src, framesIn * mFrameSize);
                            } else {
                                int16_t *src16 = (int16_t *)src;
                                int16_t *dst16 = (int16_t *)dst;
                                if (mChannelCount == 1) {
                                    while (framesIn--) {
                                        *dst16++ = *src16;
                                        *dst16++ = *src16++;
                                    }
                                } else {
                                    while (framesIn--) {
                                        *dst16++ = (int16_t)(((int32_t)*src16 + (int32_t)*(src16 + 1)) >> 1);
                                        src16 += 2;
                                    }
                                }
                            }
                        }
                        if (framesOut && mFrameCount == mRsmpInIndex) {
                            ssize_t bytesRead;
                            if (framesOut == mFrameCount &&
                                (mChannelCount == mReqChannelCount || mFormat != AudioSystem::PCM_16_BIT)) {
                                bytesRead = mInput->read(buffer.raw, mInputBytes);
                                framesOut = 0;
                            } else {
                                bytesRead = mInput->read(mRsmpInBuffer, mInputBytes);
                                mRsmpInIndex = 0;
                            }
                            if (bytesRead < 0) {
                                LOGE("Error reading audio input");
                                sleep(1);
                                mRsmpInIndex = mFrameCount;
                                framesOut = 0;
                                buffer.frameCount = 0;
                            }
                        }
                    }
                } else {
                    // resampling

                    memset(mRsmpOutBuffer, 0, framesOut * 2 * sizeof(int32_t));
                    // alter output frame count as if we were expecting stereo samples
                    if (mChannelCount == 1 && mReqChannelCount == 1) {
                        framesOut >>= 1;
                    }
                    mResampler->resample(mRsmpOutBuffer, framesOut, this);
                    // ditherAndClamp() works as long as all buffers returned by mActiveTrack->getNextBuffer()
                    // are 32 bit aligned which should be always true.
                    if (mChannelCount == 2 && mReqChannelCount == 1) {
                        AudioMixer::ditherAndClamp(mRsmpOutBuffer, mRsmpOutBuffer, framesOut);
                        // the resampler always outputs stereo samples: do post stereo to mono conversion
                        int16_t *src = (int16_t *)mRsmpOutBuffer;
                        int16_t *dst = buffer.i16;
                        while (framesOut--) {
                            *dst++ = (int16_t)(((int32_t)*src + (int32_t)*(src + 1)) >> 1);
                            src += 2;
                        }
                    } else {
                        AudioMixer::ditherAndClamp((int32_t *)buffer.raw, mRsmpOutBuffer, framesOut);
                    }

                }
                mActiveTrack->releaseBuffer(&buffer);
                mActiveTrack->overflow();
            }
            // client isn't retrieving buffers fast enough
            else {
                if (!mActiveTrack->setOverflow())
                    LOGW("RecordThread: buffer overflow");
                // Release the processor for a while before asking for a new buffer.
                // This will give the application more chance to read from the buffer and
                // clear the overflow.
                usleep(5000);
            }
        }
    }

    if (!mStandby) {
        mInput->standby();
    }
    mActiveTrack.clear();

    sendConfigEvent(AudioSystem::INPUT_CLOSED);
    processConfigEvents();

    LOGV("RecordThread %p exiting", this);
    return false;
}

status_t AudioFlinger::RecordThread::start(RecordThread::RecordTrack* recordTrack)
{
    LOGV("RecordThread::start");
    AutoMutex lock(&mLock);

    if (mActiveTrack != 0) {
        if (recordTrack != mActiveTrack.get()) return -EBUSY;

        if (mActiveTrack->mState == TrackBase::PAUSING) mActiveTrack->mState = TrackBase::RESUMING;

        return NO_ERROR;
    }

    mActiveTrack = recordTrack;
    mActiveTrack->mState = TrackBase::RESUMING;
    // signal thread to start
    LOGV("Signal record thread");
    mWaitWorkCV.signal();
    mStartStopCond.wait(mLock);
    if (mActiveTrack != 0) {
        LOGV("Record started OK");
        return NO_ERROR;
    } else {
        LOGV("Record failed to start");
        return BAD_VALUE;
    }
}

void AudioFlinger::RecordThread::stop(RecordThread::RecordTrack* recordTrack) {
    LOGV("RecordThread::stop");
    AutoMutex lock(&mLock);
    if (mActiveTrack != 0 && recordTrack == mActiveTrack.get()) {
        mActiveTrack->mState = TrackBase::PAUSING;
        mStartStopCond.wait(mLock);
    }
}

status_t AudioFlinger::RecordThread::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    pid_t pid = 0;

    if (mActiveTrack != 0 && mActiveTrack->mClient != 0) {
        snprintf(buffer, SIZE, "Record client pid: %d\n", mActiveTrack->mClient->pid());
        result.append(buffer);
    } else {
        result.append("No record client\n");
    }
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioFlinger::RecordThread::getNextBuffer(AudioBufferProvider::Buffer* buffer)
{
    size_t framesReq = buffer->frameCount;
    size_t framesReady = mFrameCount - mRsmpInIndex;
    int channelCount;

    if (framesReady == 0) {
        ssize_t bytesRead = mInput->read(mRsmpInBuffer, mInputBytes);
        if (bytesRead < 0) {
            LOGE("RecordThread::getNextBuffer() Error reading audio input");
            sleep(1);
            buffer->raw = 0;
            buffer->frameCount = 0;
            return NOT_ENOUGH_DATA;
        }
        mRsmpInIndex = 0;
        framesReady = mFrameCount;
    }

    if (framesReq > framesReady) {
        framesReq = framesReady;
    }

    if (mChannelCount == 1 && mReqChannelCount == 2) {
        channelCount = 1;
    } else {
        channelCount = 2;
    }
    buffer->raw = mRsmpInBuffer + mRsmpInIndex * channelCount;
    buffer->frameCount = framesReq;
    return NO_ERROR;
}

void AudioFlinger::RecordThread::releaseBuffer(AudioBufferProvider::Buffer* buffer)
{
    mRsmpInIndex += buffer->frameCount;
    buffer->frameCount = 0;
}

bool AudioFlinger::RecordThread::checkForNewParameters_l()
{
    bool reconfig = false;

    while (!mNewParameters.isEmpty()) {
        status_t status = NO_ERROR;
        String8 keyValuePair = mNewParameters[0];
        AudioParameter param = AudioParameter(keyValuePair);
        int value;
        int reqFormat = mFormat;
        int reqSamplingRate = mReqSampleRate;
        int reqChannelCount = mReqChannelCount;

        mNewParameters.removeAt(0);

        if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
            reqSamplingRate = value;
            reconfig = true;
        }
        if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
            reqFormat = value;
            reconfig = true;
        }
        if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
            reqChannelCount = AudioSystem::popCount(value);
            reconfig = true;
        }
        if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
            // do not accept frame count changes if tracks are open as the track buffer
            // size depends on frame count and correct behavior would not be garantied
            // if frame count is changed after track creation
            if (mActiveTrack != 0) {
                status = INVALID_OPERATION;
            } else {
                reconfig = true;
            }
        }
        if (status == NO_ERROR) {
            status = mInput->setParameters(keyValuePair);
            if (status == INVALID_OPERATION) {
               mInput->standby();
               status = mInput->setParameters(keyValuePair);
            }
            if (reconfig) {
                if (status == BAD_VALUE &&
                    reqFormat == mInput->format() && reqFormat == AudioSystem::PCM_16_BIT &&
                    ((int)mInput->sampleRate() <= 2 * reqSamplingRate) &&
                    (AudioSystem::popCount(mInput->channels()) < 3) && (reqChannelCount < 3)) {
                    status = NO_ERROR;
                }
                if (status == NO_ERROR) {
                    readInputParameters();
                    sendConfigEvent_l(AudioSystem::INPUT_CONFIG_CHANGED);
                }
            }
        }
        mParamStatus = status;
        mParamCond.signal();
        mWaitWorkCV.wait(mLock);
    }
    return reconfig;
}

String8 AudioFlinger::RecordThread::getParameters(const String8& keys)
{
    return mInput->getParameters(keys);
}

void AudioFlinger::RecordThread::audioConfigChanged(int event, int param) {
    AudioSystem::OutputDescriptor desc;
    void *param2 = 0;

    switch (event) {
    case AudioSystem::INPUT_OPENED:
    case AudioSystem::INPUT_CONFIG_CHANGED:
        desc.channels = mChannelCount;
        desc.samplingRate = mSampleRate;
        desc.format = mFormat;
        desc.frameCount = mFrameCount;
        desc.latency = 0;
        param2 = &desc;
        break;

    case AudioSystem::INPUT_CLOSED:
    default:
        break;
    }
    mAudioFlinger->audioConfigChanged(event, this, param2);
}

void AudioFlinger::RecordThread::readInputParameters()
{
    if (mRsmpInBuffer) delete mRsmpInBuffer;
    if (mRsmpOutBuffer) delete mRsmpOutBuffer;
    if (mResampler) delete mResampler;
    mResampler = 0;

    mSampleRate = mInput->sampleRate();
    mChannelCount = AudioSystem::popCount(mInput->channels());
    mFormat = mInput->format();
    mFrameSize = mInput->frameSize();
    mInputBytes = mInput->bufferSize();
    mFrameCount = mInputBytes / mFrameSize;
    mRsmpInBuffer = new int16_t[mFrameCount * mChannelCount];

    if (mSampleRate != mReqSampleRate && mChannelCount < 3 && mReqChannelCount < 3)
    {
        int channelCount;
         // optmization: if mono to mono, use the resampler in stereo to stereo mode to avoid
         // stereo to mono post process as the resampler always outputs stereo.
        if (mChannelCount == 1 && mReqChannelCount == 2) {
            channelCount = 1;
        } else {
            channelCount = 2;
        }
        mResampler = AudioResampler::create(16, channelCount, mReqSampleRate);
        mResampler->setSampleRate(mSampleRate);
        mResampler->setVolume(AudioMixer::UNITY_GAIN, AudioMixer::UNITY_GAIN);
        mRsmpOutBuffer = new int32_t[mFrameCount * 2];

        // optmization: if mono to mono, alter input frame count as if we were inputing stereo samples
        if (mChannelCount == 1 && mReqChannelCount == 1) {
            mFrameCount >>= 1;
        }

    }
    mRsmpInIndex = mFrameCount;
}

// ----------------------------------------------------------------------------

int AudioFlinger::openOutput(uint32_t *pDevices,
                                uint32_t *pSamplingRate,
                                uint32_t *pFormat,
                                uint32_t *pChannels,
                                uint32_t *pLatencyMs,
                                uint32_t flags)
{
    status_t status;
    PlaybackThread *thread = NULL;
    mHardwareStatus = AUDIO_HW_OUTPUT_OPEN;
    uint32_t samplingRate = pSamplingRate ? *pSamplingRate : 0;
    uint32_t format = pFormat ? *pFormat : 0;
    uint32_t channels = pChannels ? *pChannels : 0;
    uint32_t latency = pLatencyMs ? *pLatencyMs : 0;

    LOGV("openOutput(), Device %x, SamplingRate %d, Format %d, Channels %x, flags %x",
            pDevices ? *pDevices : 0,
            samplingRate,
            format,
            channels,
            flags);

    if (pDevices == NULL || *pDevices == 0) {
        return 0;
    }
    Mutex::Autolock _l(mLock);

    AudioStreamOut *output = mAudioHardware->openOutputStream(*pDevices,
                                                             (int *)&format,
                                                             &channels,
                                                             &samplingRate,
                                                             &status);
    LOGV("openOutput() openOutputStream returned output %p, SamplingRate %d, Format %d, Channels %x, status %d",
            output,
            samplingRate,
            format,
            channels,
            status);

    mHardwareStatus = AUDIO_HW_IDLE;
    if (output != 0) {
        if ((flags & AudioSystem::OUTPUT_FLAG_DIRECT) ||
            (format != AudioSystem::PCM_16_BIT) ||
            (channels != AudioSystem::CHANNEL_OUT_STEREO)) {
            thread = new DirectOutputThread(this, output);
            LOGV("openOutput() created direct output: ID %d thread %p", (mNextThreadId + 1), thread);
        } else {
            thread = new MixerThread(this, output);
            LOGV("openOutput() created mixer output: ID %d thread %p", (mNextThreadId + 1), thread);
        }
        mPlaybackThreads.add(++mNextThreadId, thread);

        if (pSamplingRate) *pSamplingRate = samplingRate;
        if (pFormat) *pFormat = format;
        if (pChannels) *pChannels = channels;
        if (pLatencyMs) *pLatencyMs = thread->latency();
    }

    return mNextThreadId;
}

int AudioFlinger::openDuplicateOutput(int output1, int output2)
{
    Mutex::Autolock _l(mLock);
    MixerThread *thread1 = checkMixerThread_l(output1);
    MixerThread *thread2 = checkMixerThread_l(output2);

    if (thread1 == NULL || thread2 == NULL) {
        LOGW("openDuplicateOutput() wrong output mixer type for output %d or %d", output1, output2);
        return 0;
    }


    DuplicatingThread *thread = new DuplicatingThread(this, thread1);
    thread->addOutputTrack(thread2);
    mPlaybackThreads.add(++mNextThreadId, thread);
    return mNextThreadId;
}

status_t AudioFlinger::closeOutput(int output)
{
    PlaybackThread *thread;
    {
        Mutex::Autolock _l(mLock);
        thread = checkPlaybackThread_l(output);
        if (thread == NULL) {
            return BAD_VALUE;
        }

        LOGV("closeOutput() %d", output);

        if (thread->type() == PlaybackThread::MIXER) {
            for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
                if (mPlaybackThreads.valueAt(i)->type() == PlaybackThread::DUPLICATING) {
                    DuplicatingThread *dupThread = (DuplicatingThread *)mPlaybackThreads.valueAt(i).get();
                    dupThread->removeOutputTrack((MixerThread *)thread);
                }
            }
        }
        mPlaybackThreads.removeItem(output);
    }
    thread->exit();

    return NO_ERROR;
}

status_t AudioFlinger::suspendOutput(int output)
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);

    if (thread == NULL) {
        return BAD_VALUE;
    }

    LOGV("suspendOutput() %d", output);
    thread->suspend();

    return NO_ERROR;
}

status_t AudioFlinger::restoreOutput(int output)
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);

    if (thread == NULL) {
        return BAD_VALUE;
    }

    LOGV("restoreOutput() %d", output);

    thread->restore();

    return NO_ERROR;
}

int AudioFlinger::openInput(uint32_t *pDevices,
                                uint32_t *pSamplingRate,
                                uint32_t *pFormat,
                                uint32_t *pChannels,
                                uint32_t acoustics)
{
    status_t status;
    RecordThread *thread = NULL;
    uint32_t samplingRate = pSamplingRate ? *pSamplingRate : 0;
    uint32_t format = pFormat ? *pFormat : 0;
    uint32_t channels = pChannels ? *pChannels : 0;
    uint32_t reqSamplingRate = samplingRate;
    uint32_t reqFormat = format;
    uint32_t reqChannels = channels;

    if (pDevices == NULL || *pDevices == 0) {
        return 0;
    }
    Mutex::Autolock _l(mLock);

    AudioStreamIn *input = mAudioHardware->openInputStream(*pDevices,
                                                             (int *)&format,
                                                             &channels,
                                                             &samplingRate,
                                                             &status,
                                                             (AudioSystem::audio_in_acoustics)acoustics);
    LOGV("openInput() openInputStream returned input %p, SamplingRate %d, Format %d, Channels %x, acoustics %x, status %d",
            input,
            samplingRate,
            format,
            channels,
            acoustics,
            status);

    // If the input could not be opened with the requested parameters and we can handle the conversion internally,
    // try to open again with the proposed parameters. The AudioFlinger can resample the input and do mono to stereo
    // or stereo to mono conversions on 16 bit PCM inputs.
    if (input == 0 && status == BAD_VALUE &&
        reqFormat == format && format == AudioSystem::PCM_16_BIT &&
        (samplingRate <= 2 * reqSamplingRate) &&
        (AudioSystem::popCount(channels) < 3) && (AudioSystem::popCount(reqChannels) < 3)) {
        LOGV("openInput() reopening with proposed sampling rate and channels");
        input = mAudioHardware->openInputStream(*pDevices,
                                                 (int *)&format,
                                                 &channels,
                                                 &samplingRate,
                                                 &status,
                                                 (AudioSystem::audio_in_acoustics)acoustics);
    }

    if (input != 0) {
         // Start record thread
        thread = new RecordThread(this, input, reqSamplingRate, reqChannels);
        mRecordThreads.add(++mNextThreadId, thread);
        LOGV("openInput() created record thread: ID %d thread %p", mNextThreadId, thread);
        if (pSamplingRate) *pSamplingRate = reqSamplingRate;
        if (pFormat) *pFormat = format;
        if (pChannels) *pChannels = reqChannels;

        input->standby();
    }

    return mNextThreadId;
}

status_t AudioFlinger::closeInput(int input)
{
    RecordThread *thread;
    {
        Mutex::Autolock _l(mLock);
        thread = checkRecordThread_l(input);
        if (thread == NULL) {
            return BAD_VALUE;
        }

        LOGV("closeInput() %d", input);
        mRecordThreads.removeItem(input);
    }
    thread->exit();

    return NO_ERROR;
}

status_t AudioFlinger::setStreamOutput(uint32_t stream, int output)
{
    Mutex::Autolock _l(mLock);
    MixerThread *dstThread = checkMixerThread_l(output);
    if (dstThread == NULL) {
        LOGW("setStreamOutput() bad output id %d", output);
        return BAD_VALUE;
    }

    LOGV("setStreamOutput() stream %d to output %d", stream, output);

    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        PlaybackThread *thread = mPlaybackThreads.valueAt(i).get();
        if (thread != dstThread &&
            thread->type() != PlaybackThread::DIRECT) {
            MixerThread *srcThread = (MixerThread *)thread;
            SortedVector < sp<MixerThread::Track> > tracks;
            SortedVector < wp<MixerThread::Track> > activeTracks;
            srcThread->getTracks(tracks, activeTracks, stream);
            if (tracks.size()) {
                dstThread->putTracks(tracks, activeTracks);
            }
            dstThread->sendConfigEvent(AudioSystem::STREAM_CONFIG_CHANGED, stream);
        }
    }

    return NO_ERROR;
}

// checkPlaybackThread_l() must be called with AudioFlinger::mLock held
AudioFlinger::PlaybackThread *AudioFlinger::checkPlaybackThread_l(int output) const
{
    PlaybackThread *thread = NULL;
    if (mPlaybackThreads.indexOfKey(output) >= 0) {
        thread = (PlaybackThread *)mPlaybackThreads.valueFor(output).get();
    }
    return thread;
}

// checkMixerThread_l() must be called with AudioFlinger::mLock held
AudioFlinger::MixerThread *AudioFlinger::checkMixerThread_l(int output) const
{
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread != NULL) {
        if (thread->type() == PlaybackThread::DIRECT) {
            thread = NULL;
        }
    }
    return (MixerThread *)thread;
}

// checkRecordThread_l() must be called with AudioFlinger::mLock held
AudioFlinger::RecordThread *AudioFlinger::checkRecordThread_l(int input) const
{
    RecordThread *thread = NULL;
    if (mRecordThreads.indexOfKey(input) >= 0) {
        thread = (RecordThread *)mRecordThreads.valueFor(input).get();
    }
    return thread;
}

// ----------------------------------------------------------------------------

status_t AudioFlinger::onTransact(
        uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnAudioFlinger::onTransact(code, data, reply, flags);
}

// ----------------------------------------------------------------------------

void AudioFlinger::instantiate() {
    defaultServiceManager()->addService(
            String16("media.audio_flinger"), new AudioFlinger());
}

}; // namespace android
