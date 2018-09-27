_FILENAME=${0##*/}
CUR_DIR=${0/${_FILENAME}}
CUR_DIR=$(cd $(dirname ${CUR_DIR}); pwd)/$(basename ${CUR_DIR})/
FLAGS=-flto -O3 -std=gnu11 -fno-stack-protector -ffunction-sections -fdata-sections -Wl,--gc-sections -lm
DEFS=-DNDEBUG

pushd $CUR_DIR/..

gcc ${FLAGS} ${DEFS} -o mp4mux_stream_x86  src/mp4mux.c  -Dmp4mux_test -DMP4E_CAN_USE_RANDOM_FILE_ACCESS=0
gcc ${FLAGS} ${DEFS} -o mp4mux_file_x86  src/mp4mux.c -Dmp4mux_test -DMP4E_CAN_USE_RANDOM_FILE_ACCESS=1
gcc ${FLAGS} ${DEFS} -o mp4demux_x86  src/mp4demux.c   -Dmp4demux_test
gcc ${FLAGS} ${DEFS} -o mp4transcode_test_x86  test/mp4transcode_test.c src/mp4mux.c src/mp4demux.c -Isrc
