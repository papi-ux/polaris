# NVENC driver floor harness

A prepared FFmpeg archive decides the minimum NVIDIA driver every Polaris host needs for
hardware encoding. libavcodec compiles that requirement in from the ffnvcodec headers it was
built against and then refuses, at runtime, to talk to any driver reporting an older NVENC API.
A host below the line loses NVENC and falls back to VAAPI or software, which is #650.

`cmake/dependencies/prepared_ffmpeg.cmake` refuses at configure time to pin an archive above
`POLARIS_MAX_SUPPORTED_FFNVCODEC_VERSION`, and `tests/cmake/test_prepared_ffmpeg.cmake` covers
that guard. This harness is the other half: it measures the floor of a real archive on a real
host, including one whose own driver is far newer than the floor being tested.

`fake_nvenc.c` builds a `libnvidia-encode.so.1` that reports whichever NVENC API version you
ask for and forwards everything else to the real driver library. `nvenc_floor_probe.c` opens
`h264_nvenc` and says what happened.

## Running it

Needs an NVIDIA host with a working CUDA runtime, and a prepared FFmpeg archive to test.

```bash
tag=v2026.713.132551
mkdir -p "$tag" && curl -sL \
  "https://github.com/LizardByte/build-deps/releases/download/$tag/Linux-x86_64-ffmpeg.tar.gz" \
  | tar xz -C "$tag"

gcc -shared -fPIC -o libnvidia-encode.so.1 fake_nvenc.c -ldl
mkdir -p stub && mv libnvidia-encode.so.1 stub/

PKG_CONFIG_PATH="$PWD/$tag/ffmpeg/lib/pkgconfig" \
  gcc -o "probe-$tag" nvenc_floor_probe.c -I "$tag/ffmpeg/include" -L "$PWD/$tag/ffmpeg/lib" \
      $(PKG_CONFIG_PATH="$PWD/$tag/ffmpeg/lib/pkgconfig" pkg-config --static --libs \
        libavcodec libswscale libavutil) -lstdc++

export POLARIS_REAL_NVENC=/lib64/libnvidia-encode.so.1
for version in 12.0 13.0 13.1; do
  printf '%s: ' "$version"
  LD_LIBRARY_PATH="$PWD/stub" POLARIS_FAKE_NVENC_VERSION="$version" "./probe-$tag"
done
```

`POLARIS_REAL_NVENC` must be the absolute path of the real library, or the stub finds itself
through `LD_LIBRARY_PATH` and recurses.

## What it measured for #650

On pc-papi, an RTX 4090 on driver 610.57.04, against the two archives either side of the pin
the 1.4.1 release moved:

| Driver reports | v2026.724.203728 (ffnvcodec 13.1) | v2026.713.132551 (ffnvcodec 13.0) |
| --- | --- | --- |
| NVENC 12.0 | failed | failed |
| NVENC 12.2 | failed | failed |
| NVENC 13.0 | failed | **opened** |
| NVENC 13.1 | opened | opened |

The failing cell prints the reporter's two lines verbatim, `Driver does not support the required
nvenc API version. Required: 13.1 Found: 13.0` and `The minimum required Nvidia driver for nvenc
is 610.00 or newer`, and returns `-38`, which is the `ENOSYS` their log shows as `Function not
implemented`. Both archives open normally against the host's own driver, so the rollback costs
nothing on a current one.
