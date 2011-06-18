//
// Urho3D Engine
// Copyright (c) 2008-2011 Lasse ��rni
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "Audio.h"
#include "Context.h"
#include "CoreEvents.h"
#include "Graphics.h"
#include "GraphicsEvents.h"
#include "Log.h"
#include "Profiler.h"
#include "Sound.h"
#include "SoundSource3D.h"

#ifndef USE_SDL
#define DIRECTSOUND_VERSION 0x0800
#include <Windows.h>
#include <MMSystem.h>
#include <dsound.h>
#else
#include <SDL.h>
#endif

#include "DebugNew.h"

static const int AUDIO_FPS = 100;

#ifndef USE_SDL
/// Audio implementation. Contains the DirectSound buffer
class AudioImpl
{
    friend class Audio;
    
public:
    /// Construct
    AudioImpl() :
        dsObject_(0),
        dsBuffer_(0)
    {
    }
    
private:
    /// DirectSound interface
    IDirectSound* dsObject_;
    /// DirectSound buffer
    IDirectSoundBuffer* dsBuffer_;
};
#else
static void SDLAudioCallback(void *userdata, Uint8 *stream, int len);
#endif

OBJECTTYPESTATIC(Audio);

Audio::Audio(Context* context) :
    Object(context),
    #ifndef USE_SDL
    impl_(new AudioImpl()),
    windowHandle_(0),
    #endif
    playing_(false),
    bufferSamples_(0),
    bufferSize_(0),
    sampleSize_(0),
    listenerPosition_(Vector3::ZERO),
    listenerRotation_(Quaternion::IDENTITY)
{
    SubscribeToEvent(E_RENDERUPDATE, HANDLER(Audio, HandleRenderUpdate));
    
    for (unsigned i = 0; i < MAX_SOUND_TYPES; ++i)
        masterGain_[i] = 1.0f;
    
    #ifndef USE_SDL
    SubscribeToEvent(E_SCREENMODE, HANDLER(Audio, HandleScreenMode));
    // Try to initialize right now, but skip if screen mode is not yet set
    Initialize();
    #else
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    #endif
}

Audio::~Audio()
{
    Release();
    
    #ifndef USE_SDL
    if (impl_->dsObject_)
    {
        impl_->dsObject_->Release();
        impl_->dsObject_ = 0;
    }
    
    delete impl_;
    impl_ = 0;
    #else
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    #endif
}

bool Audio::SetMode(int bufferLengthMSec, int mixRate, bool sixteenBit, bool stereo, bool interpolate)
{
    Release();
    
    #ifndef USE_SDL
    if (!impl_->dsObject_)
    {
        if (DirectSoundCreate(0, &impl_->dsObject_, 0) != DS_OK)
        {
            LOGERROR("Could not create DirectSound object");
            return false;
        }
    }
    
    if (impl_->dsObject_->SetCooperativeLevel((HWND)windowHandle_, DSSCL_PRIORITY) != DS_OK)
    {
        LOGERROR("Could not set DirectSound cooperative level");
        return false;
    }
    DSCAPS dsCaps;
    dsCaps.dwSize = sizeof(dsCaps);
    if (impl_->dsObject_->GetCaps(&dsCaps) != DS_OK)
    {
        LOGERROR("Could not get DirectSound capabilities");
        return false;
    }
    
    if (!(dsCaps.dwFlags & (DSCAPS_SECONDARY16BIT|DSCAPS_PRIMARY16BIT)))
        sixteenBit = false;
    if (!(dsCaps.dwFlags & (DSCAPS_SECONDARYSTEREO|DSCAPS_PRIMARYSTEREO)))
        stereo = false;
    
    bufferLengthMSec = Max(bufferLengthMSec, 50);
    mixRate = Clamp(mixRate, 11025, 48000);
    
    WAVEFORMATEX waveFormat;
    waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat.nSamplesPerSec = mixRate;
    
    if (sixteenBit)
        waveFormat.wBitsPerSample = 16;
    else
        waveFormat.wBitsPerSample = 8;
    
    if (stereo)
        waveFormat.nChannels = 2;
    else
        waveFormat.nChannels = 1;
    
    unsigned sampleSize = waveFormat.nChannels * waveFormat.wBitsPerSample / 8;
    unsigned numSamples = (bufferLengthMSec * mixRate) / 1000;
    
    waveFormat.nAvgBytesPerSec = mixRate * sampleSize;
    waveFormat.nBlockAlign = sampleSize;
    waveFormat.cbSize = 0;
    
    DSBUFFERDESC bufferDesc;
    memset(&bufferDesc, 0, sizeof(bufferDesc));
    bufferDesc.dwSize = sizeof(bufferDesc);
    bufferDesc.dwFlags = DSBCAPS_STICKYFOCUS;
    bufferDesc.dwBufferBytes = numSamples * sampleSize;
    bufferDesc.lpwfxFormat = &waveFormat;
    
    if (impl_->dsObject_->CreateSoundBuffer(&bufferDesc, &impl_->dsBuffer_, 0) != DS_OK)
    {
        LOGERROR("Could not create DirectSound buffer");
        return false;
    }
    
    clipBuffer_ = new int[numSamples * waveFormat.nChannels];
    
    bufferSamples_ = numSamples;
    bufferSize_ = numSamples * sampleSize;
    sampleSize_ = sampleSize;
    mixRate_ = mixRate;
    sixteenBit_ = sixteenBit;
    stereo_ = stereo;
    interpolate_ = interpolate;
    #else
    SDL_AudioSpec desired;
    SDL_AudioSpec obtained;
    
    desired.freq = mixRate;
    desired.format = AUDIO_U8;
    if (sixteenBit)
        desired.format = AUDIO_S16SYS;
    desired.channels = 1;
    if (stereo)
        desired.channels = 2;
    
    // For SDL, do not actually use the buffer length, but calculate a suitable power-of-two size from the mixrate
    if (desired.freq <= 11025)
        desired.samples = 512;
    else if (desired.freq <= 22050)
        desired.samples = 1024;
    else if (desired.freq <= 44100)
        desired.samples = 2048;
    else
        desired.samples = 4096;
    
    desired.callback = SDLAudioCallback;
    desired.userdata = this;
    
    SDL_PauseAudio(1);
    playing_ = false;
    if (SDL_OpenAudio(&desired, &obtained))
    {
        LOGERROR("Could not initialize audio output");
        return false;
    }
    
    sampleSize_ = 1;
    if (obtained.channels == 2)
    {
        stereo_ = true;
        sampleSize_ <<= 1;
    }
    if ((obtained.format == AUDIO_S16SYS) || (obtained.format == AUDIO_S16LSB) || (obtained.format == AUDIO_S16MSB))
    {
        sixteenBit_ = true;
        sampleSize_ <<= 1;
    }
    
    clipBuffer_ = new int[obtained.samples * obtained.channels];
    
    bufferSamples_ = obtained.samples;
    bufferSize_ = bufferSamples_ * sampleSize_;
    mixRate_ = obtained.freq;
    interpolate_ = interpolate;
    #endif
    
    LOGINFO("Set audio mode " + String(mixRate_) + " Hz " + (stereo_ ? "stereo" : "mono") + " " +
        (sixteenBit_ ? "16-bit" : "8-bit") + " " + (interpolate_ ? "interpolated" : ""));
    
    return Play();
}

void Audio::Update(float timeStep)
{
    PROFILE(UpdateAudio);
    
    MutexLock Lock(audioMutex_);
    
    // Update in reverse order, because sound sources might remove themselves
    for (unsigned i = soundSources_.Size() - 1; i < soundSources_.Size(); --i)
        soundSources_[i]->Update(timeStep);
}

bool Audio::Play()
{
    if (playing_)
        return true;
    
    #ifndef USE_SDL
    if (!impl_->dsBuffer_)
    {
        LOGERROR("No audio buffer, can not start playback");
        return false;
    }
    
    // Clear buffer before starting playback
    DWORD bytes1, bytes2;
    void *ptr1, *ptr2;
    unsigned char value = sixteenBit_ ? 0 : 128;
    if (impl_->dsBuffer_->Lock(0, bufferSize_, &ptr1, &bytes1, &ptr2, &bytes2, 0) == DS_OK)
    {
        if (bytes1)
            memset(ptr1, value, bytes1);
        if (bytes2)
            memset(ptr2, value, bytes2);
        impl_->dsBuffer_->Unlock(ptr1, bytes1, ptr2, bytes2);
    }
    
    // Create playback thread
    if (!Start())
    {
        LOGERROR("Could not create audio thread");
        return false;
    }
    
    // Adjust playback thread priority
    SetPriority(THREAD_PRIORITY_ABOVE_NORMAL);
    playing_ = true;
    #else
    if (!clipBuffer_)
    {
        LOGERROR("No audio buffer, can not start playback");
        return false;
    }
    SDL_PauseAudio(0);
    playing_ = true;
    #endif
    
    return true;
}

void Audio::Stop()
{
    #ifndef USE_SDL
    Thread::Stop();
    #else
    if (playing_)
        SDL_PauseAudio(1);
    #endif
    playing_ = false;
}

void Audio::SetMasterGain(SoundType type, float gain)
{
    if (type >= MAX_SOUND_TYPES)
        return;
    
    masterGain_[type] = Clamp(gain, 0.0f, 1.0f);
}

void Audio::SetListenerPosition(const Vector3& position)
{
    listenerPosition_ = position;
}

void Audio::SetListenerRotation(const Quaternion& rotation)
{
    listenerRotation_ = rotation;
}

void Audio::SetListenerTransform(const Vector3& position, const Quaternion& rotation)
{
    listenerPosition_ = position;
    listenerRotation_ = rotation;
}

void Audio::StopSound(Sound* soundClip)
{
    for (PODVector<SoundSource*>::Iterator i = soundSources_.Begin(); i != soundSources_.End(); ++i)
    {
        if ((*i)->GetSound() == soundClip)
            (*i)->Stop();
    }
}

bool Audio::IsInitialized() const
{
    #ifndef USE_SDL
    return impl_->dsBuffer_ != 0;
    #else
    return clipBuffer_.GetPtr() != 0;
    #endif
}

float Audio::GetMasterGain(SoundType type) const
{
    if (type >= MAX_SOUND_TYPES)
        return 0.0f;
    
    return masterGain_[type];
}

void Audio::AddSoundSource(SoundSource* channel)
{
    MutexLock Lock(audioMutex_);
    
    soundSources_.Push(channel);
}

void Audio::RemoveSoundSource(SoundSource* channel)
{
    MutexLock Lock(audioMutex_);
    
    for (PODVector<SoundSource*>::Iterator i = soundSources_.Begin(); i != soundSources_.End(); ++i)
    {
        if (*i == channel)
        {
            soundSources_.Erase(i);
            return;
        }
    }
}

#ifndef USE_SDL
void Audio::ThreadFunction()
{
    AudioImpl* impl = impl_;
    
    DWORD playCursor = 0;
    DWORD writeCursor = 0;
    
    while (shouldRun_)
    {
        Timer audioUpdateTimer;
        
        // Restore buffer / restart playback if necessary
        DWORD status;
        impl->dsBuffer_->GetStatus(&status);
        if (status == DSBSTATUS_BUFFERLOST)
        {
            impl->dsBuffer_->Restore();
            impl->dsBuffer_->GetStatus(&status);
        }
        if (!(status & DSBSTATUS_PLAYING))
        {
            impl->dsBuffer_->Play(0, 0, DSBPLAY_LOOPING);
            writeCursor = 0;
        }
        
        // Get current buffer position
        impl->dsBuffer_->GetCurrentPosition(&playCursor, 0);
        playCursor %= bufferSize_;
        playCursor &= -((int)sampleSize_);
        
        if (playCursor != writeCursor)
        {
            int writeBytes = playCursor - writeCursor;
            if (writeBytes < 0)
                writeBytes += bufferSize_;
            
            // Try to lock buffer
            DWORD bytes1, bytes2;
            void *ptr1, *ptr2;
            if (impl->dsBuffer_->Lock(writeCursor, writeBytes, &ptr1, &bytes1, &ptr2, &bytes2, 0) == DS_OK)
            {
                // Mix sound to locked positions
                {
                    MutexLock Lock(audioMutex_);
                    
                    if (bytes1)
                        MixOutput(ptr1, bytes1);
                    if (bytes2)
                        MixOutput(ptr2, bytes2);
                }
                
                // Unlock buffer and update write cursor
                impl->dsBuffer_->Unlock(ptr1, bytes1, ptr2, bytes2);
                writeCursor += writeBytes;
                if (writeCursor >= bufferSize_)
                    writeCursor -= bufferSize_;
            }
        }
        
        // Sleep the remaining time of the audio update period
        int audioSleepTime = Max(1000 / AUDIO_FPS - (int)audioUpdateTimer.GetMSec(false), 0);
        Sleep(audioSleepTime);
    }
    
    impl->dsBuffer_->Stop();
}
#else
void SDLAudioCallback(void *userdata, Uint8* stream, int len)
{
    Audio* audio = static_cast<Audio*>(userdata);
    
    {
        MutexLock Lock(audio->GetMutex());
        audio->MixOutput(stream, len);
    }
}
#endif

void Audio::MixOutput(void *dest, unsigned bytes)
{
    unsigned mixSamples = bytes;
    unsigned clipSamples = bytes;
    
    if (stereo_)
        mixSamples >>= 1;
    
    if (sixteenBit_)
    {
        clipSamples >>= 1;
        mixSamples >>= 1;
    }
    
    // Clear clip buffer
    memset(clipBuffer_.GetPtr(), 0, clipSamples * sizeof(int));
    int* clipPtr = clipBuffer_.GetPtr();
    
    // Mix samples to clip buffer
    // If the total work request is too large, Ogg Vorbis decode buffers may end up wrapping. Divide to smaller chunks if necessary
    unsigned maxSamples = mixRate_ * DECODE_BUFFER_LENGTH / 1000 / 4;
    while (mixSamples)
    {
        unsigned currentSamples = Min((int)maxSamples, (int)mixSamples);
        for (PODVector<SoundSource*>::Iterator i = soundSources_.Begin(); i != soundSources_.End(); ++i)
            (*i)->Mix(clipPtr, currentSamples, mixRate_, stereo_, interpolate_);
        
        mixSamples -= currentSamples;
        if (stereo_)
            clipPtr += currentSamples * 2;
        else
            clipPtr += currentSamples;
        
    }
    
    // Copy output from clip buffer to destination
    clipPtr = clipBuffer_.GetPtr();
    if (sixteenBit_)
    {
        short* destPtr = (short*)dest;
        while (clipSamples--)
            *destPtr++ = Clamp(*clipPtr++, -32768, 32767);
    }
    else
    {
        unsigned char* destPtr = (unsigned char*)dest;
        while (clipSamples--)
            *destPtr++ = Clamp(((*clipPtr++) >> 8) + 128, 0, 255);
    }
}

void Audio::HandleRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace RenderUpdate;
    
    Update(eventData[P_TIMESTEP].GetFloat());
}

#ifndef USE_SDL
void Audio::HandleScreenMode(StringHash eventType, VariantMap& eventData)
{
    if (!windowHandle_)
        Initialize();
}

void Audio::Initialize()
{
    Graphics* graphics = GetSubsystem<Graphics>();
    if ((!graphics) || (!graphics->IsInitialized()))
        return;
    
    windowHandle_ = graphics->GetWindowHandle();
}
#endif

void Audio::Release()
{
    Stop();
    
    #ifndef USE_SDL
    if (impl_->dsBuffer_)
    {
        impl_->dsBuffer_->Release();
        impl_->dsBuffer_ = 0;
    }
    #else
    if (clipBuffer_)
    {
        SDL_CloseAudio();
        clipBuffer_.Reset();
    }
    #endif
}

void RegisterAudioLibrary(Context* context)
{
    Sound::RegisterObject(context);
    SoundSource::RegisterObject(context);
    SoundSource3D::RegisterObject(context);
}
