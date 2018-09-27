[![Build Status](https://travis-ci.org/aspt/mp4.svg)](https://travis-ci.org/aspt/mp4)

Minimalistic MP4 muxer & demuxer

MP4 muxer features:
- Support audio, H.264 video and private data tracks
- Option for MP4 streaming (no fseek())

MP4 demuxer features:
- Parse MP4 headers, and provide sample sizes & offsets to the application
