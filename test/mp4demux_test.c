/** 20.09.2009 ASP @file
*
*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <memory.h>
#include "mp4demux.h"


/**
*   Print MP4 information to stdout.
*/
static void print_mp4_info(const MP4D_demux_t * mp4_demux)
{
    unsigned i;

    printf("\nMP4 FILE: %d tracks found. Movie time %.2f sec\n", mp4_demux->track_count, (4294967296.0*mp4_demux->duration_hi + mp4_demux->duration_lo) / mp4_demux->timescale);
    printf("\nNo|type|lng| duration           | bitrate| %-23s| Object type","Stream type");
    for (i = 0; i < mp4_demux->track_count; i++)
    {
        MP4D_track_t * tr = mp4_demux->track + i;

        printf("\n%2d|%c%c%c%c|%c%c%c|%7.2f s %6d frm| %7d|", i,
            (tr->handler_type>>24), (tr->handler_type>>16), (tr->handler_type>>8), (tr->handler_type>>0),
            tr->language[0],tr->language[1],tr->language[2],
            (65536.0* 65536.0*tr->duration_hi + tr->duration_lo) / tr->timescale,
            tr->sample_count,
            tr->avg_bitrate_bps
            );

        printf(" %-23s|", MP4D__stream_type_to_ascii(tr->stream_type));
        printf(" %-23s", MP4D__object_type_to_ascii(tr->object_type_indication));

        if (tr->handler_type == MP4_HANDLER_TYPE_SOUN)
        {
            printf("  -  %d ch %d hz", tr->SampleDescription.audio.channelcount, tr->SampleDescription.audio.samplerate_hz);
        }
        else if (tr->handler_type == MP4_HANDLER_TYPE_VIDE)
        {
            printf("  -  %dx%d", tr->SampleDescription.video.width, tr->SampleDescription.video.height);
        }
    }
    printf("\n");
}

/**
*   Print MP4 file comment to stdout.
*/
static void print_comment(const MP4D_demux_t * mp4_demux)
{
#define STR_TAG(name) if (mp4_demux->tag.name)  printf("%10s = %s\n", #name, mp4_demux->tag.name)
    STR_TAG(title);
    STR_TAG(artist);
    STR_TAG(album);
    STR_TAG(year);
    STR_TAG(comment);
    STR_TAG(genre);
}

/**
*   Print SPS/PPS/DSI data in hex to stdout.
*/
static void print_dsi_data(const MP4D_demux_t * mp4_demux)
{
    unsigned k, ntrack;
    for (ntrack = 0; ntrack < mp4_demux->track_count; ntrack++)
    {
        MP4D_track_t *tr = mp4_demux->track + ntrack;
        if (tr->dsi_bytes)
        {
            int i, k, sps_bytes, pps_bytes, sps_pps_found = 0;
            for (i = 0; i < 256; i++)
            {
                const unsigned char *sps = MP4D__read_sps(mp4_demux, ntrack, i, &sps_bytes);
                const unsigned char *pps = MP4D__read_pps(mp4_demux, ntrack, i, &pps_bytes);
                if (sps && sps_bytes)
                {
                    printf("%d SPS bytes found for track #%d:\n", sps_bytes, ntrack);
                    for (k = 0; k < sps_bytes; k++)
                    {
                        printf("%02x ", sps[k]);
                    }
                    printf("\n");
                    sps_pps_found = 1;
                }
                if (pps && pps_bytes)
                {
                    printf("%d PPS bytes found for track #%d:\n", pps_bytes, ntrack);
                    for (k = 0; k < pps_bytes; k++)
                    {
                        printf("%02x ", pps[k]);
                    }
                    printf("\n");
                    sps_pps_found = 1;
                }
            }

            if (!sps_pps_found)
            {
                printf("%d DSI bytes found for track #%d:\n", tr->dsi_bytes, ntrack);
                for (k = 0; k < tr->dsi_bytes; k++)
                {
                    printf("%02x ", tr->dsi[k]);
                }
                printf("\n");
            }
        }
    }
}

/**
*   Save AVC & audio tracks data to files
*/
static void save_track_data(const MP4D_demux_t * mp4_demux, FILE * mp4_file, unsigned ntrack)
{
    unsigned i, frame_bytes, timestamp, duration;
    unsigned avc_bytes_to_next_nal = 0;
    MP4D_track_t *tr = mp4_demux->track + ntrack;
    FILE * track_file;
    char name[100];
    const char * ext =  (tr->object_type_indication == MP4_OBJECT_TYPE_AVC) ? "264" : 
        (tr->handler_type == MP4_HANDLER_TYPE_SOUN) ? "audio" :
        (tr->handler_type == MP4_HANDLER_TYPE_VIDE) ? "video" : "data";

    sprintf(name, "track%d.%s", ntrack, ext);
    track_file = fopen(name,"wb");
    for (i = 0; i < tr->sample_count; i++)
    {
        unsigned char * frame_mem;
        mp4d_size_t frame_ofs = MP4D__frame_offset(mp4_demux, ntrack, i, &frame_bytes, &timestamp, &duration);

        // print frame offset
        //printf("%4d %06x %08d %d\n", i, (unsigned)ofs, duration, frame_bytes);
        //printf("%4d %06x %08d %d\n", i, (unsigned)ofs, timestamp, frame_bytes);

        // save payload
        frame_mem = malloc(frame_bytes);
        fseek(mp4_file, (long)frame_ofs, SEEK_SET);
        fread(frame_mem, 1, frame_bytes, mp4_file);

        if (mp4_demux->track[ntrack].object_type_indication == MP4_OBJECT_TYPE_AVC)
        {
            // replace 4-byte length field with start code
            unsigned startcode = 0x01000000;
            unsigned k = avc_bytes_to_next_nal;
            while (k < frame_bytes - 4)
            {
                avc_bytes_to_next_nal = 4 + ((frame_mem[k] * 256 + frame_mem[k+1])*256 + frame_mem[k+2])*256 + frame_mem[k+3];
                *(unsigned *)(frame_mem + k) = startcode;
                k += avc_bytes_to_next_nal;
            }
            avc_bytes_to_next_nal = k - frame_bytes;

            // Write SPS/PPS for 1st frame
            if (!i)
            {
                const void * data;
                int nps, bytes; 
                for (nps = 0; NULL != (data = MP4D__read_sps(mp4_demux, ntrack, nps, &bytes)); nps++)
                {
                    fwrite(&startcode, 1, 4, track_file);
                    fwrite(data, 1, bytes, track_file);
                }
                for (nps = 0; NULL != (data = MP4D__read_pps(mp4_demux, ntrack, nps, &bytes)); nps++)
                {
                    fwrite(&startcode, 1, 4, track_file);
                    fwrite(data, 1, bytes, track_file);
                }
            }
        }

        fwrite(frame_mem, 1, frame_bytes, track_file);
        free(frame_mem);
    }
    fclose(track_file);
}

int main(int argc, char* argv[])
{
    unsigned ntrack = 0;
    MP4D_demux_t mp4_demux = {0,};
    char* file_name = (argc>1)?argv[1]:"default_input.mp4";
    FILE * mp4_file = fopen(file_name, "rb");

    printf("\n\n\n%s\n\n", file_name); fflush(stdout);
    if (!mp4_file)
    {
        printf("\nERROR: can't open file %s for reading\n", file_name);
        return 0;
    }
    if (!MP4D__open(&mp4_demux, mp4_file))
    {
        printf("\nERROR: can't parse %s \n", file_name);
        return 0;
    }

    print_mp4_info(&mp4_demux);
    
    for (ntrack = 0; ntrack < mp4_demux.track_count; ntrack++)
    {
        save_track_data(&mp4_demux, mp4_file, ntrack);
    }

    print_comment(&mp4_demux);
    print_dsi_data(&mp4_demux);
    MP4D__close(&mp4_demux);
    fclose(mp4_file);

    return 0;
}
