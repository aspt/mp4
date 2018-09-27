_FILENAME=${0##*/}
CUR_DIR=${0/${_FILENAME}}
CUR_DIR=$(cd $(dirname ${CUR_DIR}); pwd)/$(basename ${CUR_DIR})/

pushd $CUR_DIR/..

./mp4mux_stream_x86 mp4mux_stream.mp4
if ! cmp ./mp4mux_stream.mp4 vectors/ref/mp4mux_stream.mp4 >/dev/null 2>&1
then
    echo test failed
    exit 1
fi

./mp4mux_file_x86 mp4mux_file.mp4
if ! cmp ./mp4mux_file.mp4 vectors/ref/mp4mux_file.mp4 >/dev/null 2>&1
then
    echo test failed
    exit 1
fi

./mp4demux_x86 mp4mux_stream.mp4
if ! cmp ./track0.audio vectors/ref/track0.audio >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
if ! cmp ./track1.264 vectors/ref/track1.264 >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
if ! cmp ./track2.data vectors/ref/track2.data >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
rm track0.audio
rm track1.264
rm track2.data

./mp4demux_x86 mp4mux_file.mp4
if ! cmp ./track0.audio vectors/ref/track0.audio >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
if ! cmp ./track1.264 vectors/ref/track1.264 >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
if ! cmp ./track2.data vectors/ref/track2.data >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
rm track0.audio
rm track1.264
rm track2.data

./mp4transcode_x86 mp4mux_file.mp4 mp4mux_stream.mp4
if ! cmp ./transcoded.mp4 vectors/ref/transcoded.mp4 >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
rm mp4mux_stream.mp4
rm mp4mux_file.mp4
rm transcoded.mp4


qemu-arm ./mp4mux_stream_arm_gcc mp4mux_stream.mp4
if ! cmp ./mp4mux_stream.mp4 vectors/ref/mp4mux_stream.mp4 >/dev/null 2>&1
then
    echo test failed
    exit 1
fi

qemu-arm ./mp4mux_file_arm_gcc mp4mux_file.mp4
if ! cmp ./mp4mux_file.mp4 vectors/ref/mp4mux_file.mp4 >/dev/null 2>&1
then
    echo test failed
    exit 1
fi

qemu-arm ./mp4demux_arm_gcc mp4mux_stream.mp4
if ! cmp ./track0.audio vectors/ref/track0.audio >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
if ! cmp ./track1.264 vectors/ref/track1.264 >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
if ! cmp ./track2.data vectors/ref/track2.data >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
rm track0.audio
rm track1.264
rm track2.data

qemu-arm ./mp4demux_arm_gcc mp4mux_file.mp4
if ! cmp ./track0.audio vectors/ref/track0.audio >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
if ! cmp ./track1.264 vectors/ref/track1.264 >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
if ! cmp ./track2.data vectors/ref/track2.data >/dev/null 2>&1
then
    echo test failed
    exit 1
fi
rm track0.audio
rm track1.264
rm track2.data

qemu-arm ./mp4transcode_arm_gcc mp4mux_file.mp4 mp4mux_stream.mp4
if ! cmp ./transcoded.mp4 vectors/ref/transcoded.mp4 >/dev/null 2>&1
then
    echo test failed
    exit 1
fi

rm mp4mux_stream.mp4
rm mp4mux_file.mp4
rm transcoded.mp4

echo test passed
