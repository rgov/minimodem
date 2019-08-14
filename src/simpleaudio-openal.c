/*
 * simpleaudio-openal.c
 *
 * Copyright (C) 2019 Ryan Govostes <rgovostes@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if USE_OPENAL

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <AL/al.h>
#include <AL/alc.h>

#include "simpleaudio.h"
#include "simpleaudio_internal.h"


/*
 * OpenAL backend for simpleaudio
 */


// we get called with very short frame sequences, so we need to
// have lots of buffer to keep things smooth
#define NUM_BUFFERS 128

typedef struct {
    ALCdevice *device;
    ALCcontext *context;
    ALuint source;
    ALuint buffers[NUM_BUFFERS];
    sa_direction_t direction;
} openal_handle;


static const char *
al_error_str( ALenum err )
{
    switch ( err ) {
    case AL_NO_ERROR:
        return "AL_NO_ERROR";
    case AL_INVALID_NAME:
        return "AL_INVALID_NAME";
    case AL_INVALID_ENUM:
        return "AL_INVALID_ENUM";
    case AL_INVALID_VALUE:
        return "AL_INVALID_VALUE";
    case AL_INVALID_OPERATION:
        return "AL_INVALID_OPERATION";
    case AL_OUT_OF_MEMORY:
        return "AL_OUT_OF_MEMORY";
    default:
        assert(0);
    }
}


static const char *
alc_error_str( ALCenum err )
{
    switch ( err ) {
    case ALC_NO_ERROR:
        return "ALC_NO_ERROR";
    case ALC_INVALID_DEVICE:
        return "ALC_INVALID_DEVICE";
    case ALC_INVALID_CONTEXT:
        return "ALC_INVALID_CONTEXT";
    case ALC_INVALID_ENUM:
        return "ALC_INVALID_ENUM";
    case ALC_INVALID_VALUE:
        return "ALC_INVALID_VALUE";
    case ALC_OUT_OF_MEMORY:
        return "ALC_OUT_OF_MEMORY";
    default:
        assert(0);
    }
}


static ALenum
al_format( sa_format_t format, int channels )
{
    assert(channels == 1 || channels == 2);

    switch ( format ) {
    case SA_SAMPLE_FORMAT_S16:
        return channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
    default:
        assert(0 && "Unsupported buffer format");
    }
}


static ssize_t
sa_openal_read( simpleaudio *sa, void *buf, size_t nframes )
{
    assert(0);
}


static ssize_t
sa_openal_write( simpleaudio *sa, void *buf, size_t nframes )
{
    openal_handle *hdl = sa->backend_handle;
    ALuint buffer = -1;

    // if we have a buffer that has never been queued, use it
    ALint nqueuedbufs = 0;
    alGetSourcei(hdl->source, AL_BUFFERS_QUEUED, &nqueuedbufs);
    assert( alGetError() == AL_NO_ERROR );

    if ( nqueuedbufs < NUM_BUFFERS ) {
        buffer = hdl->buffers[nqueuedbufs];
    }

    // otherwise, wait for a queued buffer to be processed
    if ( buffer == -1 ) {
        ALint nreadybufs = 0;
        while ( nreadybufs <= 0 ) {
            alGetSourcei(hdl->source, AL_BUFFERS_PROCESSED, &nreadybufs);
            assert( alGetError() == AL_NO_ERROR );
        }

        alSourceUnqueueBuffers(hdl->source, 1, &buffer);
        assert( alGetError() == AL_NO_ERROR );
    }

    // write the data to it
    alBufferData(
        buffer,
        al_format(simpleaudio_get_format(sa), simpleaudio_get_channels(sa)),
        buf,
        nframes * sa->backend_framesize,
        simpleaudio_get_rate(sa)
    );
    assert( alGetError() == AL_NO_ERROR );

    // enqueue the buffer to be played
    alSourceQueueBuffers(hdl->source, 1, &buffer);
    assert( alGetError() == AL_NO_ERROR );

    // ensure the source is playing
    ALint state = -1;
    alGetSourcei(hdl->source, AL_SOURCE_STATE, &state);
    assert( alGetError() == AL_NO_ERROR );

    if ( state != AL_PLAYING ) {
        alSourcePlay(hdl->source);
    }

    return nframes;
}


static void
sa_openal_close( simpleaudio *sa )
{
    openal_handle *hdl = sa->backend_handle;
    if ( !hdl ) {
        return;
    }

    // wait until we're done playing audio
    ALint state;
    do {
        alGetSourcei(hdl->source, AL_SOURCE_STATE, &state);
        assert( alGetError() == AL_NO_ERROR );
    } while ( state == AL_PLAYING );

    // delete the source
    alDeleteSources(1, &hdl->source);
    
    // destroy the context
    alcMakeContextCurrent(NULL);
    alcDestroyContext(hdl->context);

    // close the device
    switch ( hdl->direction ) {
    case SA_STREAM_PLAYBACK:
        alcCloseDevice(hdl->device);
        break;
    case SA_STREAM_RECORD:
        alcCaptureCloseDevice(hdl->device);
        break;
    default:
        assert(0);
    }

    sa->backend_handle = NULL;
}


static int
sa_openal_open_stream(
		simpleaudio *sa,
		const char *backend_device,
		sa_direction_t sa_stream_direction,
		sa_format_t sa_format,
		unsigned int rate, unsigned int channels,
		char *app_name, char *stream_name )
{
    openal_handle *hdl = malloc(sizeof(openal_handle));
    hdl->direction = sa_stream_direction;

    // open the audio device
    switch ( hdl->direction ) {
    case SA_STREAM_PLAYBACK:
        hdl->device = alcOpenDevice(backend_device);
        break;
    case SA_STREAM_RECORD:
        assert(0);
        // TODO:
        // hdl->device = alcCaptureOpenDevice(...);
        break;
    default:
        assert(0);
    }

    if ( !hdl->device ) {
        fprintf(stderr, "E: Cannot open OpenAL device: %s\n ", al_error_str(alGetError()));
        goto fail;
    }

    // create the context
    hdl->context = alcCreateContext(hdl->device, NULL);
    alcMakeContextCurrent(hdl->context);
    if ( !hdl->context ) {
        fprintf(stderr, "E: Cannot create OpenAL context: %s\n ", alc_error_str(alcGetError(hdl->device)));
        goto fail;
    }

    // create an audio source
    alGenSources(1, &hdl->source);
    assert( alGetError() == AL_NO_ERROR );

    // create buffers
    alGenBuffers(NUM_BUFFERS, &hdl->buffers);
    assert( alGetError() == AL_NO_ERROR );

    sa->backend_handle = hdl;
    sa->backend_framesize = sa->channels * sa->samplesize;

    return 1;

fail:
    free(hdl);
    return 0;
}


const struct simpleaudio_backend simpleaudio_backend_openal = {
    sa_openal_open_stream,
    sa_openal_read,
    sa_openal_write,
    sa_openal_close,
};

#endif /* USE_openal */
