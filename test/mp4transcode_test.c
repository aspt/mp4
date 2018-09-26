/** ASP 21.12.2016 @file
*  
*/

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <memory.h>
#include "mp4demux.h"
#include "mp4mux.h"
// #include "mp4mux.c"
// #include "mp4demux.c"


int transcode(int argc, char* argv[])
{
    unsigned i, ntrack = 0;
    int ninput;
    MP4E_mux_t * mux =  MP4E__open(fopen("transcoded.mp4", "wb"));

    for (ninput = 0; ninput < 2; ninput++)
    {
        MP4D_demux_t mp4 = {0,};
        char * file_name = (argc>1+ninput)?argv[1+ninput]:"input.mp4";
        FILE * input_file = fopen(file_name, "rb");
        if (!input_file)
        {
            printf("\ncant open %s\n", file_name);
            break;
        }
        MP4D__open(&mp4, input_file);

        for (ntrack = 0; ntrack < mp4.track_count; ntrack++)
//        for (ntrack = mp4.track_count - 1; ntrack >= 0; ntrack--)
        {
            MP4D_track_t *tr = mp4.track + ntrack;
            unsigned sum_duration = 0;
            MP4E_track_t tre;
            int trid;

            tre.object_type_indication = tr->object_type_indication;

            // Take video track from 1st file and audio from 2nd
            if (!(
            tr->handler_type == MP4_HANDLER_TYPE_VIDE && ninput == 0 ||
            tr->handler_type == MP4_HANDLER_TYPE_SOUN && ninput == 1 
            )
            )
            {
                continue;
            }

            memcpy(tre.language, tr->language, 4);
            if (tr->handler_type == MP4_HANDLER_TYPE_VIDE)
            {
                tre.track_media_kind = e_video;
                tre.u.v.width = tr->SampleDescription.video.width;
                tre.u.v.height = tr->SampleDescription.video.height;
            }
            if (tr->handler_type == MP4_HANDLER_TYPE_SOUN)
            {
                tre.track_media_kind = e_audio;
                tre.u.a.channelcount = tr->SampleDescription.audio.channelcount;
            }
            if (tr->handler_type == MP4_HANDLER_TYPE_GESM)
            {
                tre.track_media_kind = e_private;
            }
            tre.time_scale = tr->timescale;
            tre.default_duration = tr->duration_lo;

            trid  = MP4E__add_track(mux, &tre);

            if (mp4.track[ntrack].object_type_indication == MP4_OBJECT_TYPE_AVC)
            {
                int sps_bytes, nsps;
                const void * sps;
                for (nsps = 0; NULL != (sps = MP4D__read_sps(&mp4, ntrack, nsps, &sps_bytes)); nsps++)
                {
                    MP4E__set_sps(mux, trid, sps, sps_bytes);
                }
                for (nsps = 0; NULL != (sps = MP4D__read_pps(&mp4, ntrack, nsps, &sps_bytes)); nsps++)
                {
                    MP4E__set_pps(mux, trid, sps, sps_bytes);
                }
            }
            else
            {
                MP4E__set_dsi(mux, trid, tr->dsi, tr->dsi_bytes);
            }
#define MAX_FRAMES ~0u
            for (i = 0; i < mp4.track[ntrack].sample_count && (tr->handler_type != MP4_HANDLER_TYPE_VIDE || i < MAX_FRAMES); i++)
            {
                int sample_kind = MP4E_SAMPLE_DEFAULT;
                unsigned frame_bytes, timestamp, duration;
                mp4d_size_t ofs = MP4D__frame_offset(&mp4, ntrack, i, &frame_bytes, &timestamp, &duration);
                unsigned char *mem = malloc(frame_bytes);
                sum_duration += duration;
                fseek(input_file, (long)ofs, SEEK_SET);
                fread(mem, 1, frame_bytes, input_file);

                if (!i || (tr->handler_type == MP4_HANDLER_TYPE_SOUN))
                {
                    sample_kind  = MP4E_SAMPLE_RANDOM_ACCESS;
                }

                // Ensure video duration is > 1 sec, extending last video frame duration
                if ((i == mp4.track[ntrack].sample_count-1 || 
                     i == (MAX_FRAMES-1)
                    )
                    && (tr->handler_type == MP4_HANDLER_TYPE_VIDE))
                {
                    if (sum_duration < tr->timescale)
                    {
                        duration += 100 + tr->timescale - sum_duration;
                    }
                }

                MP4E__put_sample(mux, trid, mem, frame_bytes, duration, sample_kind);
                free(mem);
            }
        }
        fclose(input_file);
        MP4D__close(&mp4);
    }

    MP4E__set_text_comment(mux, "transcoded");
    MP4E__close(mux);
    return 0;
}


int main(int argc, char* argv[])
{
    return transcode(argc, argv);
}
