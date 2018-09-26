/** 24.04.2010 ASP @file
*
*   ISO/IEC 14496-12:2005
*
**/      

#ifndef MP4MUX_H_INCLUDED
#define MP4MUX_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif  //__cplusplus

#include <stdio.h>
#include "mp4defs.h"


/************************************************************************/
/*          API error codes                                             */
/************************************************************************/
#define MP4E_STATUS_OK                       0
#define MP4E_STATUS_BAD_ARGUMENTS           -1
#define MP4E_STATUS_NO_MEMORY               -2
#define MP4E_STATUS_FILE_WRITE_ERROR        -3
#define MP4E_STATUS_ONLY_ONE_DSI_ALLOWED    -4


/************************************************************************/
/*          Sample kind for MP4E__put_sample()                          */
/************************************************************************/
#define MP4E_SAMPLE_DEFAULT             0   // (beginning of) audio or video frame 
#define MP4E_SAMPLE_RANDOM_ACCESS       1   // mark sample as random access point (key frame)

/************************************************************************/
/*          Data structures                                             */
/************************************************************************/

typedef struct MP4E_mux_tag MP4E_mux_t;

typedef struct
{
    // MP4 object type code, which defined codec class for the track. 
    // See MP4E_OBJECT_TYPE_* values for some codecs
    unsigned object_type_indication;

    // Track language: 3-char ISO 639-2T code: "und", "eng", "rus", "jpn" etc...
    unsigned char language[4];

    enum
    {
        e_audio,
        e_video,
        e_private
    } track_media_kind;

    // 90000 for video, sample rate for audio
    unsigned time_scale;

    // default sample duration: 
    // for ex: time_scale / FPS - for fixed FPS video, of frame size for audio
    // default duration can be overriden by MP4E__put_sample() parameter
    unsigned default_duration;

    union
    {
        struct  
        {
            // number of channels in the audio track.
            unsigned channelcount;
        } a;

        struct 
        {
            int width;
            int height;
        } v;
    } u;
} MP4E_track_t;


/************************************************************************/
/*          API                                                         */
/************************************************************************/

/**
*   Allocates and initialize mp4 multiplexer.
*   Passed file handle owned by the multiplexor, and closed with MP4E__close()
*
*   return multiplexor handle on success; NULL on failure
*
*   Example:
*
*        MP4E_mux_t * mux = MP4E__open(fopen(input_file_name, "wb"));
*/
MP4E_mux_t * MP4E__open(FILE * mp4file);


/**
*   Add new track 
*   The track_data parameter does not referred by the multiplexer after function 
*   return, and may be allocated in short-time memory. The dsi member of 
*   track_data parameter is mandatory.
*
*   return ID of added track, or error code MP4E_STATUS_*
*
*   Example 1:  add AVC video track:
*
*       MP4E_track_t track = {0,};
*       track.object_type_indication = MP4E_OBJECT_TYPE_AVC;
*       track.track_media_kind = e_video;
*       track.time_scale = 90000;
*       track.default_duration = track.time_scale / 30; // 30 FPS
*       track.u.v.width = 640;
*       track.u.v.height = 640;
*       video_track_id = MP4E__add_track(mux, &track);
*
*
*   Example 2:  add AAC audio track:
*
*       MP4E_track_t track = {0,};
*       track.object_type_indication = MP4E_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_LC_PROFILE;
*       track.track_media_kind = e_audio;
*       track.time_scale = 48000;       // sample rate, hz
*       track.default_duration = 1024;  // AAC frame size
*       track.u.a.channelcount = 2;     // stereo
*       audio_track_id = MP4E__add_track(mux, &track);
*/
int MP4E__add_track(MP4E_mux_t * mux, const MP4E_track_t * track_data);


/**
*   Add new sample to specified track
*   The tracks numbered starting with 0, according to order of MP4E__add_track() calls
*   'kind' is one of MP4E_SAMPLE_... defines
*   non-zero 'duration' overrides MP4E_track_t::default_duration settings
*
*   return error code MP4E_STATUS_*
*
*   Example 1: put 'P' video frame: 
*
*       MP4E__put_sample(mux, video_track_id, data, data_bytes, frame_duration, MP4E_SAMPLE_DEFAULT);
*
*   Example 2: put AAC frame: 
*
*       MP4E__put_sample(mux, audio_track_id, data, data_bytes, 0, MP4E_SAMPLE_RANDOM_ACCESS);
*/
int MP4E__put_sample(MP4E_mux_t * mux, int track_id, const void * data, int data_bytes, int duration, int kind);


/**
*   Finalize MP4 file, de-allocated memory, and closes MP4 multiplexer. 
*   The close operation takes a time and disk space, since it writes MP4 file 
*   indexes.  Please note that this function does not closes file handle, 
*   which was passed to open function.
*
*   return error code MP4E_STATUS_*
*/
int MP4E__close(MP4E_mux_t * mux);


/**
*   Set Decoder Specific Info (DSI)
*   Can be used for audio and private tracks.
*   MUST be used for AAC track.
*   Only one DSI can be set. It is an error to set DSI again 
*
*   return error code MP4E_STATUS_*
*
*   Example: 
*
*       MP4E__set_dsi(mux, aac_track_id, dsi, dsi_bytes);
*
*/
int MP4E__set_dsi(MP4E_mux_t * mux, int track_id, const void * dsi, int bytes);


/**
*   Set SPS/PPS data. MUST be used for AVC (H.264) track. 
*   Up to 32 different SPS can be used in one track.
*   Up to 256 different PPS can be used in one track.
*
*   return error code MP4E_STATUS_*
*
*/
int MP4E__set_sps(MP4E_mux_t * mux, int track_id, const void * sps, int bytes);
int MP4E__set_pps(MP4E_mux_t * mux, int track_id, const void * pps, int bytes);


/**
*   Set or replace ASCII test comment for the file. Set comment to NULL to remove comment.
*
*   return error code MP4E_STATUS_*
*
*   Example: 
*
*       MP4E__set_text_comment(mux, "file comment");
*
*/
int MP4E__set_text_comment(MP4E_mux_t * mux, const char * comment);


#ifdef __cplusplus
}
#endif //__cplusplus

#endif //MP4MUX_H_INCLUDED
