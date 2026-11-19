# Synthetic video fixtures

These AVC streams contain only locally generated solid-blue frames. They do
not contain console firmware or game assets. FFmpeg with libx264 generated them:

```text
ffmpeg -f lavfi -i color=c=blue:s=16x18:r=1 -frames:v 1 -c:v libx264 -preset ultrafast -tune zerolatency -x264-params keyint=1:repeat-headers=1 -f h264 vdecsw-blue16x18.h264
ffmpeg -f lavfi -i color=c=blue:s=16x18:r=8 -frames:v 8 -c:v libx264 -preset medium -x264-params keyint=30:bframes=2:b-adapt=0:aud=1:repeat-headers=1 -f h264 vdecsw-bframes16x18.h264
```

`test_vdecsw` checks the real decoder's NV12 pixels, macroblock cropping,
separate input/output queues, full-queue status and retry, packet metadata through
B-frame reordering, end-of-sequence draining, reset, ownership and handles.
The second stream includes access-unit delimiters to separate its eight packets.
