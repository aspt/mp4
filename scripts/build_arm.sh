_FILENAME=${0##*/}
CUR_DIR=${0/${_FILENAME}}
CUR_DIR=$(cd $(dirname ${CUR_DIR}); pwd)/$(basename ${CUR_DIR})/
FLAGS=-static -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard -flto -O3 -std=gnu11 -ffast-math -fomit-frame-pointer -ftree-vectorize -lm
DEFS=-DNDEBUG

pushd $CUR_DIR/..

gcc ${FLAGS} ${DEFS} -o mp4mux_stream_arm_gcc  src/mp4mux.c  -Dmp4mux_test -DMP4E_CAN_USE_RANDOM_FILE_ACCESS=0
gcc ${FLAGS} ${DEFS} -o mp4mux_file_arm_gcc  src/mp4mux.c -Dmp4mux_test -DMP4E_CAN_USE_RANDOM_FILE_ACCESS=1
gcc ${FLAGS} ${DEFS} -o mp4demux_arm_gcc  src/mp4demux.c   -Dmp4demux_test
gcc ${FLAGS} ${DEFS} -o mp4transcode_test_arm_gcc  test/mp4transcode_test.c src/mp4mux.c src/mp4demux.c -Isrc
