/** 24.04.2010 ASP @file
*
*   Ref: [1] ISO/IEC 14496-12:2005
*
**/      
#include "mp4mux.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/************************************************************************/
/*      Build config                                                    */
/************************************************************************/
// if fseek() avaialable, use single MDAT atom per file, and update data size on close
#ifndef MP4E_CAN_USE_RANDOM_FILE_ACCESS
#define MP4E_CAN_USE_RANDOM_FILE_ACCESS    0
#endif

// How much memory needed for indexes
// Experimental data:
// file with 1 track = 560 bytes
// file with 2 tracks = 972 bytes
// track size = 412 bytes;
// file header size = 148 bytes
#define FILE_HEADER_BYTES 256       // file header
#define TRACK_HEADER_BYTES 512      // track header

// File timescale
#define MOOV_TIMESCALE 1000

typedef unsigned int mp4e_size_t;

/*
*   Sample descriptor
*   1 sample = 1 video frame (incl all slices)
*            = 1 audio frame
*/
typedef struct 
{
    mp4e_size_t size;               // sample data size
    mp4e_size_t offset;             // sample data offset in the mp4 file 
    unsigned duration;              // sample duration, x(1./MP4E_track_t::time_scale) seconds
    unsigned flag_random_access;    // 1 if sample intra-coded
} sample_t;

/*
*   Dynamically sized memory block ('vector')
*/
typedef struct 
{
    unsigned char * data;           // malloc()-ed memory
    size_t bytes;                   // used size
    size_t capacity;                // allocated size
} asp_vector_t;

/*
*   Track descriptor
*   Track is a sequence of samples. There are 1 or several tracks in the mp4 file
*/
typedef struct 
{
    MP4E_track_t info;              // Application-supplied track description
    asp_vector_t smpl;              // samples descriptor
    asp_vector_t vsps;              // SPS for video or DSI for audio
    asp_vector_t vpps;              // PPS for video, not used for audio
} track_t;

/*
*   MP4 file descriptor
*/
typedef struct MP4E_mux_tag
{
    asp_vector_t tracks;            // mp4 file tracks
    FILE * mp4file;                 // output file handle
    mp4e_size_t write_pos;          // ## of bytes written ~ current file position (until 1st fseek)
    char * text_comment;            // application-supplied file comment

#if MP4E_CAN_USE_RANDOM_FILE_ACCESS
    mp4e_size_t mdat_start;         // MDAT box start position
#endif
} MP4E_mux_t;



/*
*   Endian-independent byte-write macros
*/
#define MP4_WR(x, n) *write_ptr++ = (unsigned char)((x) >> 8*n)
#define WR1(x) MP4_WR(x,0);
#define WR2(x) MP4_WR(x,1); MP4_WR(x,0);
#define WR3(x) MP4_WR(x,2); MP4_WR(x,1); MP4_WR(x,0);
#define WR4(x) MP4_WR(x,3); MP4_WR(x,2); MP4_WR(x,1); MP4_WR(x,0);
// esc-coded OD length
#define MP4_WRITE_OD_LEN(size) if (size > 0x7F) do { size -= 0x7F; WR1(0x00ff);} while (size > 0x7F); WR1(size)

#define MP4_WRITE_STRING(s) {size_t len = strlen(s) + 1; memcpy(write_ptr, s, len); write_ptr += len;}

#define MP4_WR4_PTR(p,x) (p)[0]=(char)((x) >> 8*3); (p)[1]=(char)((x) >> 8*2); (p)[2]=(char)((x) >> 8*1); (p)[3]=(char)((x));

// Finish atom: update atom size field
#define MP4_END_ATOM --stack; MP4_WR4_PTR((unsigned char*)*stack, write_ptr - *stack);}

// Initiate atom: save position of size field on stack
#define MP4_ATOM(x)  {*stack++ = write_ptr; write_ptr += 4; WR4(x);

// Atom with 'FullAtomVersionFlags' field
#define MP4_FULL_ATOM(x, flag)  MP4_ATOM(x); WR4(flag);


/************************************************************************/
/*      File output (non-portable) stuff                                */
/************************************************************************/

/**
*   Data stream output function
*/
static size_t mp4e_fwrite(MP4E_mux_t * mux, const void *buffer, size_t size)
{
    mux->write_pos += size;
    return fwrite(buffer, size, 1, mux->mp4file);
}

/************************************************************************/
/*      Abstract vector data structure                                  */
/************************************************************************/
/**
*   Allocate vector with given size, return 1 on success, 0 on fail
*/
static int asp_vector_init(asp_vector_t * h, int capacity)
{
    h->bytes = 0;
    h->capacity = capacity;
    h->data = capacity ? (unsigned char *)malloc(capacity) : NULL;
    return !capacity || !!h->data;
}

/**
*   Deallocates vector memory 
*/
static void asp_vector_reset(asp_vector_t * h)
{
    free(h->data);
    memset(h, 0, sizeof(asp_vector_t));
}

/**
*   Allocates given number of bytes at the end of vector data, increasing
*   vector memory if necessary.
*   Return allocated memory.
*/
static unsigned char * asp_vector_alloc_tail(asp_vector_t * h, size_t bytes)
{
    unsigned char * p;
    if (h->bytes + bytes > h->capacity)
    {
        size_t grow_by = (bytes > h->capacity/2 ? bytes : h->capacity/2);
        size_t new_size = (h->capacity + grow_by + 1024) & -1024; // less-realloc's variant
        p = (unsigned char *)realloc(h->data, new_size);
        if (!p)
        {
            return NULL;
        }
        h->data = p;
        h->capacity = new_size;
    }
    assert(h->capacity - h->bytes >= bytes);
    p = h->data + h->bytes;
    h->bytes += bytes;
    return p;
}

/**
*   Append data to the end of the vector (accumulate or enqueue)
*/
static unsigned char * asp_vector_put(asp_vector_t * h, const void * buf, int bytes)
{
    unsigned char * tail = asp_vector_alloc_tail(h, bytes);
    if (tail)
    {
        memcpy(tail, buf, bytes);
    }
    return tail;
}


/************************************************************************/
/*      Exported API functions                                          */
/************************************************************************/

/**
*   Destroy multiplexer object: release memory, close output file handle
*/
static void mp4e_free(MP4E_mux_t * mux)
{
    if (mux) 
    {
        unsigned long ntr, ntracks = mux->tracks.bytes / sizeof(track_t);
        for (ntr = 0; ntr < ntracks; ntr++)
        {
            track_t* tr = ((track_t*)mux->tracks.data) + ntr;
            asp_vector_reset(&tr->vsps);
            asp_vector_reset(&tr->vpps);
            asp_vector_reset(&tr->smpl);
        }
        asp_vector_reset(&mux->tracks);
        fclose(mux->mp4file);
        free(mux);
    }
}

/**
*   Allocates and initialize mp4 multiplexer
*   return multiplexer handle on success; NULL on failure
*/
MP4E_mux_t * MP4E__open(FILE * mp4file)
{
    // hardcoded part of the file header
    static const unsigned char mp4e_box_ftyp[] = 
    {
#if 1
        0,0,0,0x18,'f','t','y','p',
        'm','p','4','2',0,0,0,0,
        'm','p','4','2','i','s','o','m', 
#else
        // as in ffmpeg
        0,0,0,0x20,'f','t','y','p',
        'i','s','o','m',0,0,2,0,
        'm','p','4','1','i','s','o','m', 
        'i','s','o','2','a','v','c','1', 
#endif
    };

    MP4E_mux_t * mux;
    if (!mp4file)
    {
        return NULL;
    }

    mux = (MP4E_mux_t *)calloc(sizeof(MP4E_mux_t), 1);
    if (mux)
    {
        int success;
        mux->mp4file = mp4file;
        asp_vector_init(&mux->tracks, 2*sizeof(track_t));

        // Write fixed header: 'ftyp' box
        success = (int)mp4e_fwrite(mux, mp4e_box_ftyp, sizeof(mp4e_box_ftyp));

#if MP4E_CAN_USE_RANDOM_FILE_ACCESS
        // Save position, which would be updated with mdat box size
        mux->mdat_start = mux->write_pos;

        // Write filler, which would be updated later
        success &= mp4e_fwrite(mux, mp4e_box_ftyp, 8);
#endif
        if (!success) 
        {
            mp4e_free(mux);
            mux = NULL;
        }
    }

    return mux;
}

/**
*   Add new track, return track ID
*/
int MP4E__add_track(MP4E_mux_t * mux, const MP4E_track_t * track_data)
{
    track_t *tr;

    if (!mux || !track_data)
    {
        return MP4E_STATUS_BAD_ARGUMENTS;
    }

    tr = (track_t*)asp_vector_alloc_tail(&mux->tracks, sizeof(track_t));
    if (!tr) 
    {
        return MP4E_STATUS_NO_MEMORY;
    }
    memset(tr, 0, sizeof(track_t));
    memcpy(&tr->info, track_data, sizeof(*track_data));
    if (!asp_vector_init(&tr->smpl, 256))
    {
        return MP4E_STATUS_NO_MEMORY;
    }
    asp_vector_init(&tr->vsps, 0);
    asp_vector_init(&tr->vpps, 0);
    return (int)(mux->tracks.bytes / sizeof(track_t)) - 1;
}

/**
*   Append new SPS/PPS to the list, keeping them in MP4 format (16-bit data_size + data)
*/
static int mp4e_sps_pps_append_mem(asp_vector_t * v, const void * mem, int bytes)
{
    size_t i;
    unsigned char size[2];
    const unsigned char * p = v->data;
    for (i = 0; i + 2 < v->bytes;)
    {
        int cb = p[i]*256 + p[i+1];
        if (cb == bytes && !memcmp(p+i+2, mem, cb))
        {
            return 1;
        }
        i += 2 + cb;
    }
    size[0] = bytes >> 8;
    size[1] = bytes;
    return asp_vector_put(v, size, 2) && asp_vector_put(v, mem, bytes);
}

/**
*   Count number of SPS/PPS in the list
*/
static int mp4e_sps_pps_items_count(asp_vector_t * v)
{
    int count= 0;
    size_t i;
    const unsigned char * p = v->data;
    for (i = 0; i + 2 < v->bytes;)
    {
        int cb = p[i]*256 + p[i+1];
        count++;
        i += 2 + cb;
    }
    return count;
}

/**
*   Set track DSI. Used for audio tracks.
*/
int MP4E__set_dsi(MP4E_mux_t * mux, int track_id, const void * dsi, int bytes)
{
    track_t* tr = ((track_t*)mux->tracks.data) + track_id;
    assert(tr->info.track_media_kind == e_audio ||
           tr->info.track_media_kind == e_private);
    if (tr->vsps.bytes)
    {
        return MP4E_STATUS_ONLY_ONE_DSI_ALLOWED;   // only one DSI allowed
    }
    return mp4e_sps_pps_append_mem(&tr->vsps, dsi, bytes) ? MP4E_STATUS_OK : MP4E_STATUS_NO_MEMORY;
}

/**
*   Set track SPS. Used for AVC video tracks.
*/
int MP4E__set_sps(MP4E_mux_t * mux, int track_id, const void * sps, int bytes)
{
    track_t* tr = ((track_t*)mux->tracks.data) + track_id;
    assert(tr->info.track_media_kind == e_video);
    return mp4e_sps_pps_append_mem(&tr->vsps, sps, bytes) ? MP4E_STATUS_OK : MP4E_STATUS_NO_MEMORY;
}

/**
*   Set track PPS. Used for AVC video tracks.
*/
int MP4E__set_pps(MP4E_mux_t * mux, int track_id, const void * pps, int bytes)
{
    track_t* tr = ((track_t*)mux->tracks.data) + track_id;
    assert(tr->info.track_media_kind == e_video);
    return mp4e_sps_pps_append_mem(&tr->vpps, pps, bytes) ? MP4E_STATUS_OK : MP4E_STATUS_NO_MEMORY;
}

/**
*   Return sum of track samples duration.
*/
static unsigned mp4e_get_track_duration(const track_t * tr)
{
    unsigned i, sum_duration = 0;
    const sample_t * s = (const sample_t *)tr->smpl.data;
    for (i = 0; i < tr->smpl.bytes/sizeof(sample_t); i++)
    {
        sum_duration += s[i].duration;
    }
    return sum_duration;
}

/**
*   Append sample descriptor to the samples list
*/
static int mp4e_add_sample_descriptor(MP4E_mux_t * mux, track_t * tr, int data_bytes, int duration, int kind)
{
    sample_t smp;
    smp.size = data_bytes;
    smp.offset = (mp4e_size_t)mux->write_pos;
    smp.duration = (duration ? duration : tr->info.default_duration);
    smp.flag_random_access = (kind == MP4E_SAMPLE_RANDOM_ACCESS);
    return NULL != asp_vector_put(&tr->smpl, &smp, sizeof(sample_t));
}

/**
*   Add new sample to specified track
*/
int MP4E__put_sample(MP4E_mux_t * mux, int track_num, const void * data, int data_bytes, int duration, int kind)
{
    track_t * tr;
    if (!mux || !data || track_num*sizeof(track_t) >= mux->tracks.bytes)
    {
        return MP4E_STATUS_BAD_ARGUMENTS;
    }
    tr = ((track_t*)mux->tracks.data) + track_num;

#if !MP4E_CAN_USE_RANDOM_FILE_ACCESS
    {
        // Write each sample to a separate atom
        unsigned char write_base[8], *write_ptr = write_base;     // for WR4 macro
        WR4(data_bytes + 8);
        WR4(BOX_mdat);
        if (!mp4e_fwrite(mux, write_base, write_ptr - write_base))
        {
            return MP4E_STATUS_FILE_WRITE_ERROR;
        }
    }
#endif

    if (!mp4e_add_sample_descriptor(mux, tr, data_bytes, duration, kind))
    {
        return MP4E_STATUS_NO_MEMORY;
    }

    if (!mp4e_fwrite(mux, data, data_bytes))
    {
        return MP4E_STATUS_FILE_WRITE_ERROR;
    }

    return MP4E_STATUS_OK;
}

/**
*   calculate size of length field of OD box
*/
static int mp4e_od_size_of_size(int size)
{
    int i, size_of_size = 1;
    for (i = size; i > 0x7F; i -= 0x7F) size_of_size++;
    return size_of_size;
}

/**
*   Add or remove MP4 file text comment according to Apple specs:
*   https://developer.apple.com/library/mac/documentation/QuickTime/QTFF/Metadata/Metadata.html#//apple_ref/doc/uid/TP40000939-CH1-SW1
*   http://atomicparsley.sourceforge.net/mpeg-4files.html
*   note that ISO did not specify comment format.
*/
int MP4E__set_text_comment(MP4E_mux_t * mux, const char * comment)
{
    if (!mux)
    {
        return MP4E_STATUS_BAD_ARGUMENTS;
    }
    // replace comment
    free(mux->text_comment);
    mux->text_comment = NULL;
    if (comment)
    {
        size_t comment_size = strlen(comment) + 1;
        mux->text_comment = (char*)malloc(comment_size);
        if (mux->text_comment)
        {
            memcpy(mux->text_comment, comment, comment_size);
        }

    }
    return MP4E_STATUS_OK;
}

/**
*   Closes MP4 multiplexer
*/
int MP4E__close(MP4E_mux_t * mux)
{
    // atoms nesting stack
    unsigned char * stack_base[20];
    unsigned char ** stack = stack_base;

    // in-memory indexes
    unsigned char * write_base, * write_ptr;

    unsigned int ntr, index_bytes, ntracks;
    int i, error_code;

    if (!mux) 
    {
        return MP4E_STATUS_BAD_ARGUMENTS;
    }

    ntracks = (unsigned int)(mux->tracks.bytes / sizeof(track_t));
    index_bytes = FILE_HEADER_BYTES;
    if (mux->text_comment)
    {
        index_bytes += 128 + strlen(mux->text_comment);
    }
    for (ntr = 0; ntr < ntracks; ntr++)
    {
        track_t * tr = ((track_t*)mux->tracks.data) + ntr;
        index_bytes += TRACK_HEADER_BYTES;          // fixed amount (implementation-dependent)
        // may need extra 4 bytes for duration field + 4 bytes for worst-case random access box
        index_bytes += tr->smpl.bytes * (sizeof(sample_t) + 4 + 4) / sizeof(sample_t);
        index_bytes += tr->vsps.bytes;
        index_bytes += tr->vpps.bytes;
    }

    // Allocate index memory
    write_base = (unsigned char*)malloc(index_bytes);
    if (!write_base)
    {
        return MP4E_STATUS_NO_MEMORY;
    }
    write_ptr = write_base;

#if MP4E_CAN_USE_RANDOM_FILE_ACCESS
    {
        // update size of mdat box.
        // The only point, which requires random file access.
        // This can be avoided using "till eof" size code, but in this case indexes must be 
        // written before the mdat....
        fpos_t endpos;
        fgetpos(mux->mp4file, &endpos);
        fseek(mux->mp4file, mux->mdat_start, SEEK_SET);
        WR4(mux->write_pos - mux->mdat_start);
        WR4(BOX_mdat);
        mp4e_fwrite(mux, write_base, write_ptr - write_base);   // after that, mux->write_pos is no longer valid, and not needed
        fsetpos(mux->mp4file, &endpos);
        write_ptr = write_base;
    }
#endif

    // 
    // Write index atoms; order taken from Table 1 of [1]
    //
    MP4_ATOM(BOX_moov);
        MP4_FULL_ATOM(BOX_mvhd, 0);
        WR4(0); // creation_time
        WR4(0); // modification_time

        if (ntracks)
        {
            track_t * tr = ((track_t*)mux->tracks.data) + 0;    // take 1st track
            unsigned duration = mp4e_get_track_duration(tr);
            duration = (unsigned)(duration * 1LL * MOOV_TIMESCALE / tr->info.time_scale);
            WR4(MOOV_TIMESCALE); // duration
            WR4(duration); // duration
        }

        WR4(0x00010000); // rate 
        WR2(0x0100); // volume
        WR2(0); // reserved
        WR4(0); // reserved
        WR4(0); // reserved

        // matrix[9]
        WR4(0x00010000);WR4(0);WR4(0);
        WR4(0);WR4(0x00010000);WR4(0);
        WR4(0);WR4(0);WR4(0x40000000);

        // pre_defined[6]
        WR4(0); WR4(0); WR4(0); 
        WR4(0); WR4(0); WR4(0); 

        //next_track_ID is a non-zero integer that indicates a value to use for the track ID of the next track to be
        //added to this presentation. Zero is not a valid track ID value. The value of next_track_ID shall be
        //larger than the largest track-ID in use.
        WR4(ntracks+1); 
        MP4_END_ATOM;
    
    for (ntr = 0; ntr < ntracks; ntr++)
    {
        track_t * tr = ((track_t*)mux->tracks.data) + ntr;
        unsigned duration = mp4e_get_track_duration(tr);
        int samples_count = (int)(tr->smpl.bytes / sizeof(sample_t));
        const sample_t * sample = (const sample_t *)tr->smpl.data;
        unsigned handler_type;
        const char * handler_ascii = NULL;

        if (samples_count <= 0)
        {
            continue;   // skip empty track
        }

        switch (tr->info.track_media_kind)
        {   
            case e_audio:
                handler_type = MP4_HANDLER_TYPE_SOUN;
                handler_ascii = "SoundHandler";
                break;
            case e_video:
                handler_type = MP4_HANDLER_TYPE_VIDE;
                handler_ascii = "VideoHandler";
                break;
            case e_private:
                handler_type = MP4_HANDLER_TYPE_GESM;
                break;
            default:
                return MP4E_STATUS_BAD_ARGUMENTS;
        }

        MP4_ATOM(BOX_trak);
            MP4_FULL_ATOM(BOX_tkhd, 7); // flag: 1=trak enabled; 2=track in movie; 4=track in preview
            WR4(0);             // creation_time
            WR4(0);             // modification_time
            WR4(ntr+1);         // track_ID
            WR4(0);             // reserved
            WR4((unsigned)(duration * 1LL * MOOV_TIMESCALE / tr->info.time_scale));
            WR4(0); WR4(0); // reserved[2]
            WR2(0);             // layer
            WR2(0);             // alternate_group
            WR2(0x0100);        // volume {if track_is_audio 0x0100 else 0}; 
            WR2(0);             // reserved

            // matrix[9]
            WR4(0x00010000);WR4(0);WR4(0);
            WR4(0);WR4(0x00010000);WR4(0);
            WR4(0);WR4(0);WR4(0x40000000);

            if (tr->info.track_media_kind == e_audio || tr->info.track_media_kind == e_private)
            {
                WR4(0); // width
                WR4(0); // height
            }
            else
            {
                WR4(tr->info.u.v.width*0x10000); // width
                WR4(tr->info.u.v.height*0x10000); // height
            }
            MP4_END_ATOM;

            MP4_ATOM(BOX_mdia);
                MP4_FULL_ATOM(BOX_mdhd, 0);
                WR4(0); // creation_time
                WR4(0); // modification_time
                WR4(tr->info.time_scale);
                WR4(duration); // duration
                {
                    int lang_code = ((tr->info.language[0]&31) << 10) | ((tr->info.language[1]&31) << 5) | (tr->info.language[2]&31);
                    WR2(lang_code); // language
                }
                WR2(0); // pre_defined
                MP4_END_ATOM;

                MP4_FULL_ATOM(BOX_hdlr, 0);
                WR4(0); // pre_defined
                WR4(handler_type); // handler_type
                WR4(0); WR4(0); WR4(0); // reserved[3]
                // name is a null-terminated string in UTF-8 characters which gives a human-readable name for the track type (for debugging and inspection purposes).
                // set mdia hdlr name field to what quicktime uses. 
                // Sony smartphone may fail to decode short files w/o handler name
                if (handler_ascii)
                {
                    MP4_WRITE_STRING(handler_ascii);
                }
                else
                {
                    WR4(0);
                }
                MP4_END_ATOM;

                MP4_ATOM(BOX_minf);

                    if (tr->info.track_media_kind == e_audio)
                    {
                        // Sound Media Header Box 
                        MP4_FULL_ATOM(BOX_smhd, 0);
                        WR2(0);   // balance
                        WR2(0);   // reserved
                        MP4_END_ATOM;
                    }
                    if (tr->info.track_media_kind == e_video)
                    {
                        // mandatory Video Media Header Box 
                        MP4_FULL_ATOM(BOX_vmhd, 1);
                        WR2(0);   // graphicsmode
                        WR2(0); WR2(0); WR2(0); // opcolor[3]
                        MP4_END_ATOM;
                    }

                    MP4_ATOM(BOX_dinf);
                        MP4_FULL_ATOM(BOX_dref, 0);
                        WR4(1); // entry_count
                            // If the flag is set indicating that the data is in the same file as this box, then no string (not even an empty one)
                            // shall be supplied in the entry field.

                            // ASP the correct way to avoid supply the string, is to use flag 1 
                            // otherwise ISO reference demux crashes
                            MP4_FULL_ATOM(BOX_url, 1);
                            MP4_END_ATOM;
                        MP4_END_ATOM;
                    MP4_END_ATOM;

                    MP4_ATOM(BOX_stbl);
                        MP4_FULL_ATOM(BOX_stsd, 0);
                        WR4(1); // entry_count;

                        if (tr->info.track_media_kind == e_audio ||
                            tr->info.track_media_kind == e_private
                        )
                        {
                            // AudioSampleEntry() assume MP4E_HANDLER_TYPE_SOUN
                            unsigned box_name = (tr->info.track_media_kind == e_audio) ? BOX_mp4a : BOX_mp4s;
                            MP4_ATOM(box_name);

                            // SampleEntry
                            WR4(0); WR2(0); // reserved[6]
                            WR2(1); // data_reference_index; - this is a tag for descriptor below

                            if (tr->info.track_media_kind == e_audio)
                            {
                                // AudioSampleEntry
                                WR4(0); WR4(0); // reserved[2]
                                WR2(tr->info.u.a.channelcount); // channelcount
                                WR2(16); // samplesize
                                WR4(0);  // pre_defined+reserved
                                WR4((tr->info.time_scale << 16));  // samplerate == = {timescale of media}<<16;
                            }

                                MP4_FULL_ATOM(BOX_esds, 0);
                                if (tr->vsps.bytes > 0)
                                {
                                    int dsi_bytes = (int)(tr->vsps.bytes) - 2; //  - two bytes size field
                                    int dsi_size_size = mp4e_od_size_of_size(dsi_bytes);
                                    int dcd_bytes = dsi_bytes + dsi_size_size + 1 + (1+1+3+4+4);
                                    int dcd_size_size = mp4e_od_size_of_size(dcd_bytes);
                                    int esd_bytes = dcd_bytes + dcd_size_size + 1 + 3;

                                    WR1(3); // OD_ESD
                                    MP4_WRITE_OD_LEN(esd_bytes);
                                    WR2(0); // ES_ID(2) // TODO - what is this?
                                    WR1(0); // flags(1)

                                    WR1(4); // OD_DCD
                                    MP4_WRITE_OD_LEN(dcd_bytes);
                                    if (tr->info.track_media_kind == e_audio)
                                    {
                                        WR1(tr->info.object_type_indication); // OD_DCD
                                        //WR1(MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3); // OD_DCD
                                        WR1(5<<2); // stream_type == AudioStream
                                    }
                                    else
                                    {
                                        // http://xhelmboyx.tripod.com/formats/mp4-layout.txt
                                        WR1(208); // 208 = private video
                                        WR1(32<<2); // stream_type == user private
                                    }
                                    WR3(tr->info.u.a.channelcount * 6144/8); // bufferSizeDB in bytes, constant as in reference decoder
                                    WR4(0); // maxBitrate TODO
                                    WR4(0); // avg_bitrate_bps TODO

                                    WR1(5); // OD_DSI
                                    MP4_WRITE_OD_LEN(dsi_bytes); 
                                    for (i = 0; i < dsi_bytes; i++)
                                    {
                                        WR1(tr->vsps.data[2+i]);
                                    }
                                }
                                MP4_END_ATOM;
                            MP4_END_ATOM;
                        }

                        if (tr->info.track_media_kind == e_video)
                        {
                            unsigned int sps_count = mp4e_sps_pps_items_count(&tr->vsps);
                            unsigned int pps_count = mp4e_sps_pps_items_count(&tr->vpps);

                            MP4_ATOM(BOX_avc1);
                            // VisualSampleEntry  8.16.2
                            // extends SampleEntry
                            WR2(0); // reserved
                            WR2(0); // reserved
                            WR2(0); // reserved
                            WR2(1); // data_reference_index
                            WR2(0); // pre_defined 
                            WR2(0); // reserved
                            WR4(0); // pre_defined 
                            WR4(0); // pre_defined 
                            WR4(0); // pre_defined 
                            WR2(tr->info.u.v.width);
                            WR2(tr->info.u.v.height);
                            WR4(0x00480000); // horizresolution = 72 dpi
                            WR4(0x00480000); // vertresolution  = 72 dpi
                            WR4(0); // reserved
                            WR2(1); // frame_count 
                            for (i = 0; i < 32; i++)
                            {
                                WR1(0); //  compressorname
                            }
                            WR2(24); // depth
                            WR2(0xffff); // pre_defined

                                MP4_ATOM(BOX_avcC);
                                // AVCDecoderConfigurationRecord 5.2.4.1.1
                                WR1(1); // configurationVersion
                                WR1(tr->vsps.data[2+1]); // 
                                WR1(tr->vsps.data[2+2]); // 
                                WR1(tr->vsps.data[2+3]); // 
                                WR1(255);                 // 0xfc + NALU_len-1  
                                WR1(0xe0 | sps_count);
                                for (i = 0; i < (int)tr->vsps.bytes; i++)
                                {
                                    WR1(tr->vsps.data[i]);
                                }

                                WR1(pps_count);
                                for (i = 0; i < (int)tr->vpps.bytes; i++)
                                {
                                    WR1(tr->vpps.data[i]);
                                }

                                MP4_END_ATOM;
                            MP4_END_ATOM;
                        }
                        MP4_END_ATOM;

                        /************************************************************************/
                        /*      indexes                                                         */
                        /************************************************************************/

                        // Time to Sample Box 
                        MP4_FULL_ATOM(BOX_stts, 0);
                        {
                            unsigned char * pentry_count = write_ptr;
                            int cnt = 1, entry_count = 0;
                            WR4(0);
                            for (i = 0; i < samples_count; i++, cnt++)
                            {
                                if (i == samples_count-1 || sample[i].duration != sample[i+1].duration)
                                {
                                    WR4(cnt);
                                    WR4(sample[i].duration);
                                    cnt = 0;
                                    entry_count++;
                                }
                            }
                            MP4_WR4_PTR(pentry_count, entry_count);
                        }
                        MP4_END_ATOM;

                        // Sample To Chunk Box 
                        MP4_FULL_ATOM(BOX_stsc, 0);
                        WR4(1); // entry_count
                        WR4(1); // first_chunk;
                        WR4(1); // samples_per_chunk;
                        WR4(1); // sample_description_index;
                        MP4_END_ATOM;

                        // Sample Size Box 
                        MP4_FULL_ATOM(BOX_stsz, 0);
                        WR4(0); // sample_size  If this field is set to 0, then the samples have different sizes, and those sizes 
                                    //  are stored in the sample size table.
                        WR4(samples_count);  // sample_count;
                        for (i = 0; i < samples_count; i++)
                        {
                            WR4(sample[i].size);
                        }
                        MP4_END_ATOM;

                        // Chunk Offset Box 
                        MP4_FULL_ATOM(BOX_stco, 0);
                        WR4(samples_count);  // entry_count
                        for (i = 0; i < samples_count; i++)
                        {
                            WR4(sample[i].offset);
                        }
                        MP4_END_ATOM;

                        // Sync Sample Box 
                        {
                            int ra_count = 0;
                            for (i = 0; i < samples_count; i++)
                            {
                                ra_count += !!sample[i].flag_random_access;
                            }
                            if (ra_count != samples_count)
                            {
                                // If the sync sample box is not present, every sample is a random access point. 
                                MP4_FULL_ATOM(BOX_stss, 0);
                                WR4(ra_count);
                                for (i = 0; i < samples_count; i++)
                                {
                                    if (sample[i].flag_random_access)
                                    {
                                        WR4(i+1);
                                    }
                                }
                                MP4_END_ATOM;
                            }
                        }
                    MP4_END_ATOM;
                MP4_END_ATOM;
            MP4_END_ATOM;
        MP4_END_ATOM;
    } // tracks loop

    if (mux->text_comment)
    {
        MP4_ATOM(BOX_udta);
            MP4_FULL_ATOM(BOX_meta, 0);
                MP4_FULL_ATOM(BOX_hdlr, 0);
                    WR4(0); // pre_defined
                    WR4(MP4_HANDLER_TYPE_MDIR); // handler_type
                    WR4(0); WR4(0); WR4(0); // reserved[3]
                    WR4(0); // name is a null-terminated string in UTF-8 characters which gives a human-readable name for the track type (for debugging and inspection purposes).
                MP4_END_ATOM;
                MP4_ATOM(BOX_ilst);
                    MP4_ATOM((unsigned)BOX_ccmt);
                        MP4_ATOM(BOX_data);
                            WR4(1); // type
                            WR4(0); //lang
                            MP4_WRITE_STRING(mux->text_comment);
                        MP4_END_ATOM;
                    MP4_END_ATOM;
                MP4_END_ATOM;
            MP4_END_ATOM;
        MP4_END_ATOM;
    }
    MP4_END_ATOM;   // moov atom

    assert((unsigned)(write_ptr - write_base) <= index_bytes);

    if (!mp4e_fwrite(mux, write_base, write_ptr - write_base))
    {
        error_code = MP4E_STATUS_FILE_WRITE_ERROR;
    }
    else
    {
        error_code = MP4E_STATUS_OK;
    }
    free(write_base);
    free(mux->text_comment);
    mp4e_free(mux);

    return error_code;
}

#ifdef mp4mux_test
/******************************************************************************
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!                                                                       !!!!
!!!!                 !!!!!!!!  !!!!!!!!   !!!!!!!   !!!!!!!!               !!!!
!!!!                    !!     !!        !!            !!                  !!!!
!!!!                    !!     !!        !!            !!                  !!!!
!!!!                    !!     !!!!!!     !!!!!!!      !!                  !!!!
!!!!                    !!     !!               !!     !!                  !!!!
!!!!                    !!     !!               !!     !!                  !!!!
!!!!                    !!     !!!!!!!!   !!!!!!!      !!                  !!!!
!!!!                                                                       !!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
******************************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <memory.h>
#include "mp4mux.h"

const unsigned char avc[] = {0, 0, 1, 0xB3, 0, 0x10, 7, 0, 0, 1, 0xB6, 16, 0x60, 0x51, 0x82, 0x3D, 0xB7, 0xEF};
const unsigned char sps[] = {0x67, 0x42, 0xE0, 0x0A, 0xDA, 0x79};
const unsigned char pps[] = {0x68, 0xCE, 0x04, 0x72};
const unsigned char idr[] = {0,0,0,12,0x65, 0xB8, 0x23, 0xFF, 0xFF, 0xF0, 0xF4, 0x50, 0x00, 0x10, 0x11, 0xF8};
const unsigned char frm[] = {0,0,0,4,0x61, 0xE2, 0x3D, 0x40 };

const unsigned char aac_dsi[] = {0x12, 0x10};
const unsigned char aac[] = {
0x21, 0x10, 0x05, 0x20, 0xA4, 0x1B, 0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x37, 0xA7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x70
};

void test(FILE * output)
{
    int i, id_video, id_audio, id_private;
    static unsigned char dummy[100];
    // == Open file
    MP4E_mux_t * mp4 = MP4E__open(output);

    // == Add audio track
    MP4E_track_t track;
    track.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
    strcpy((char*)track.language, "und");
    track.u.a.channelcount = 2;
    track.time_scale = 44100;
    track.default_duration = 1024;
    track.track_media_kind = e_audio;
    id_audio = MP4E__add_track(mp4, &track);
    
    // == Add video track
    track.track_media_kind = e_video;
    track.object_type_indication = MP4_OBJECT_TYPE_AVC;
    track.time_scale = 90000;
    track.default_duration = 90000 / 30;
    track.u.v.width = 16;
    track.u.v.height = 16;
    id_video = MP4E__add_track(mp4, &track);

    // == Add private data track
    track.track_media_kind = e_private;
    track.time_scale = 44100;
    track.default_duration = 1024;
    track.object_type_indication = MP4_OBJECT_TYPE_USER_PRIVATE;
    id_private = MP4E__add_track(mp4, &track);


    // == Supply SPS/PPS/DSI descriptors
    MP4E__set_sps(mp4, id_video, sps, sizeof(sps));
    MP4E__set_pps(mp4, id_video, pps, sizeof(pps));
    MP4E__set_dsi(mp4, id_audio, aac_dsi, sizeof(aac_dsi));
    for (i = 0; i < 10; i++)
    {
        dummy[i] = 16+i;
    }
    MP4E__set_dsi(mp4, id_private, dummy, 10);


    // == Append audio data
    for (i = 0; i < 2 * 44100 / 1024; i++)
    {
        MP4E__put_sample(mp4, id_audio, aac, sizeof(aac), 0, MP4E_SAMPLE_RANDOM_ACCESS);
    }
    // == Append video data
    for (i = 0; i < 30; i++)
    {
        MP4E__put_sample(mp4, id_video, idr, sizeof(idr), 0, MP4E_SAMPLE_RANDOM_ACCESS);
        MP4E__put_sample(mp4, id_video, frm, sizeof(frm), 0, MP4E_SAMPLE_DEFAULT);
    }
    // == Append private data
    for (i = 0; i < 2 * 44100 / 1024; i++)
    {
        int bytes = 100;
        int duration = 1024;
        dummy[0] = i;
        MP4E__put_sample(mp4, id_private, dummy, bytes, duration, MP4E_SAMPLE_DEFAULT);
    }

    // == Set file comment
    MP4E__set_text_comment(mp4, "test comment");
    
    // == Close session
    MP4E__close(mp4);
}

int main(int argc, char* argv[])
{
    FILE * file;
    char * output_file_name = (argc > 1)?argv[1]:"mp4mux_test.mp4";
    if (!output_file_name)
    {
        printf("ERROR: no file name given!\n");
        return 1;
    }
    file = fopen(output_file_name, "wb");
    if (!file)
    {
        printf("ERROR: can't open file %s!\n", output_file_name);
        return 1;
    }
    test(file);

    return 0;
}

// dmc mp4mux.c -Dmp4mux_test -DMP4E_CAN_USE_RANDOM_FILE_ACCESS=1 && mp4mux.exe && del *.obj *.map mp4mux.exe
#endif  // mp4mux_test
