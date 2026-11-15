#include <MediaDefs.h>
#include <MediaRoster.h>
#include <SoundPlayer.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include "miniaudio.h"
}

/* miniaudio has no Haiku backend; this wraps the Media Kit's BSoundPlayer.
   It is a custom backend: the Media Kit thread pulls data through
   ma_device_handle_backend_data_callback(). */

namespace {

/* shortcut: one output device per process, like the rest of tit */
BSoundPlayer *g_player;

ma_format format_from_media(uint32_t format)
{
    switch (format) {
    case media_raw_audio_format::B_AUDIO_FLOAT:  return ma_format_f32;
    case media_raw_audio_format::B_AUDIO_INT:    return ma_format_s32;
    case media_raw_audio_format::B_AUDIO_SHORT:  return ma_format_s16;
    case media_raw_audio_format::B_AUDIO_UCHAR:  return ma_format_u8;
    default:                                     return ma_format_unknown;
    }
}

void fill(void *cookie, void *buffer, size_t size,
          const media_raw_audio_format &format)
{
    ma_device *device = (ma_device *)cookie;
    ma_uint32 sample_size =
        (ma_uint32)(format.format & media_raw_audio_format::B_AUDIO_SIZE_MASK);
    if (!device || !sample_size || !format.channel_count) return;
    ma_uint32 frames =
        (ma_uint32)(size / (sample_size * format.channel_count));
    if (frames)
        ma_device_handle_backend_data_callback(device, buffer, NULL, frames);
}

ma_result haiku_context_init(ma_context *, const ma_context_config *,
                             ma_backend_callbacks *)
{
    /* Without an output node there is nothing to play into; let miniaudio
       fall back to Null (a VM without a sound device, for example). */
    BMediaRoster *roster = BMediaRoster::Roster();
    media_node output;
    if (!roster || roster->GetAudioOutput(&output) != B_OK)
        return MA_FAILED_TO_OPEN_BACKEND_DEVICE;
    return MA_SUCCESS;
}

ma_result haiku_context_uninit(ma_context *)
{
    return MA_SUCCESS;
}

ma_result haiku_context_enumerate(ma_context *context,
                                  ma_enum_devices_callback_proc callback,
                                  void *user)
{
    ma_device_info info;
    memset(&info, 0, sizeof(info));
    snprintf(info.name, sizeof(info.name), "%s", "Haiku Media Kit");
    info.isDefault = MA_TRUE;
    callback(context, ma_device_type_playback, &info, user);
    return MA_SUCCESS;
}

ma_result haiku_context_get_device_info(ma_context *, ma_device_type type,
                                        const ma_device_id *,
                                        ma_device_info *info)
{
    if (type != ma_device_type_playback) return MA_NO_DEVICE;
    snprintf(info->name, sizeof(info->name), "%s", "Haiku Media Kit");
    info->isDefault = MA_TRUE;
    info->nativeDataFormats[0].format = ma_format_unknown;
    info->nativeDataFormats[0].channels = 0;
    info->nativeDataFormats[0].sampleRate = 0;
    info->nativeDataFormats[0].flags = 0;
    info->nativeDataFormatCount = 1;
    return MA_SUCCESS;
}

ma_result haiku_device_init(ma_device *device,
                            const ma_device_config *config,
                            ma_device_descriptor *playback,
                            ma_device_descriptor *capture)
{
    (void)capture;
    if (config->deviceType != ma_device_type_playback || !playback)
        return MA_DEVICE_TYPE_NOT_SUPPORTED;

    media_raw_audio_format format = media_raw_audio_format::wildcard;
    if (playback->channels)
        format.channel_count = playback->channels;
    if (playback->sampleRate)
        format.frame_rate = (float)playback->sampleRate;

    g_player = new BSoundPlayer(&format, "tit", fill, NULL, device);
    if (!g_player) return MA_OUT_OF_MEMORY;
    if (g_player->InitCheck() != B_OK) {
        delete g_player;
        g_player = NULL;
        return MA_FAILED_TO_OPEN_BACKEND_DEVICE;
    }

    media_raw_audio_format actual = g_player->Format();
    ma_format ma_fmt = format_from_media(actual.format);
    if (ma_fmt == ma_format_unknown || !actual.channel_count ||
        actual.frame_rate <= 0) {
        delete g_player;
        g_player = NULL;
        return MA_FORMAT_NOT_SUPPORTED;
    }

    ma_uint32 bytes_per_frame =
        ma_get_bytes_per_sample(ma_fmt) * actual.channel_count;
    ma_uint32 period = actual.buffer_size
        ? (ma_uint32)(actual.buffer_size / bytes_per_frame) : 1024;
    if (!period) period = 1024;

    playback->format = ma_fmt;
    playback->channels = actual.channel_count;
    playback->sampleRate = (ma_uint32)actual.frame_rate;
    playback->periodSizeInFrames = period;
    if (playback->channelMap[0] == MA_CHANNEL_NONE) {
        ma_channel_map_init_standard(ma_standard_channel_map_default,
                                     playback->channelMap,
                                     sizeof(playback->channelMap) /
                                         sizeof(playback->channelMap[0]),
                                     playback->channels);
    }
    return MA_SUCCESS;
}

ma_result haiku_device_uninit(ma_device *)
{
    delete g_player;
    g_player = NULL;
    return MA_SUCCESS;
}

ma_result haiku_device_start(ma_device *)
{
    return MA_SUCCESS;
}

ma_result haiku_device_stop(ma_device *)
{
    if (g_player) g_player->Stop(true, true);
    return MA_SUCCESS;
}

ma_result haiku_data_loop(ma_device *device)
{
    if (g_player) {
        g_player->SetHasData(true);
        if (g_player->Start() != B_OK) return MA_ERROR;
    }
    while (ma_device_get_state(device) == ma_device_state_started)
        usleep(50000);
    return MA_SUCCESS;
}

} // namespace

extern "C" ma_result tit_haiku_backend_init(ma_backend_callbacks *callbacks)
{
    memset(callbacks, 0, sizeof(*callbacks));
    callbacks->onContextInit = haiku_context_init;
    callbacks->onContextUninit = haiku_context_uninit;
    callbacks->onContextEnumerateDevices = haiku_context_enumerate;
    callbacks->onContextGetDeviceInfo = haiku_context_get_device_info;
    callbacks->onDeviceInit = haiku_device_init;
    callbacks->onDeviceUninit = haiku_device_uninit;
    callbacks->onDeviceStart = haiku_device_start;
    callbacks->onDeviceStop = haiku_device_stop;
    callbacks->onDeviceDataLoop = haiku_data_loop;
    return MA_SUCCESS;
}
