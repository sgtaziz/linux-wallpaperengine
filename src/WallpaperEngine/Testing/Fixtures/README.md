# Generated audio fixture

`audio-loop-sine.mp3` is a locally generated 997 Hz mono sine wave, 0.12 seconds at 44.1 kHz, encoded to MP3 with the system FFmpeg `libmp3lame` encoder. It contains no Workshop or third-party recording.

Generation from the repository root:

```sh
ffmpeg -hide_banner -loglevel error -f lavfi -i sine=frequency=997:sample_rate=44100:duration=0.12 -ac 1 -codec:a libmp3lame -b:a 96k src/WallpaperEngine/Testing/Fixtures/audio-loop-sine.mp3
```

The finite decode reference is 5,760 stereo float samples at 48 kHz (46,080 bytes), checked with:

```sh
ffmpeg -hide_banner -loglevel error -i src/WallpaperEngine/Testing/Fixtures/audio-loop-sine.mp3 -f f32le -ac 2 -ar 48000 - | wc -c
```
