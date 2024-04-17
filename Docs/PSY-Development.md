[Top level](../README.md)

# SVT-AV1-PSY: Additional Info

## Build Instructions

Through the sections below, we aim to provide a simplified build guide for compiling a standalone SVT-AV1-PSY binary in the form of `SvtAv1EncApp`. Instructions are provided for Linux & macOS as well as for Windows. Please note that these instructions are *just* for procuring a standalone binary; since many of our users are evaluating the standalone binary's performance or using SVT-AV1-PSY in chunked encoding workflows which utilize the standalone binary, we have not included instructions for building the SVT-AV1-PSY plugin for FFmpeg or other needs not affiliated with the standalone binary in this section.

If you are encountering issues with the processes we've documented or you are aware that you have unique needs that are not covered here (such as building FFmpeg with SVT-AV1-PSY), please refer to the more verbose [SVT-AV1 Build Guide](Build-Guide.md) that we have carried over from the mainline SVT-AV1 project.

### Linux & macOS

On Linux & macOS, you can choose to build SVT-AV1-PSY using the provided Bash script or manually with the CMake build system. Both methods for building SVT-AV1-PSY's `SvtAv1EncApp` standalone binary are detailed below.

#### Bash Script

This is the recommended method, as it conveniently runs CMake under the hood and provides generally adequate flexibility. If you just wish to build a working, optimized binary, the bash script is likely the best option for you.

0. Ensure you have the necessary dependencies for this process, which may include:
    - Git
    - CMake 3.23 or newer
    - Yasm 1.2.0 or newer
    - GCC or Clang (preferably Clang)
    - A POSIX-compliant shell (Bash, Zsh, etc.)

1. Clone the repository & enter the Linux build directory:

```bash
git clone https://github.com/gianni-rosato/svt-av1-psy
cd svt-av1-psy/Build/linux
```

2. Run the build script with the desired options. You can run `./build.sh --help` if you'd like to see the full suite of options available to you, but a sane configuration is provided below:

```bash
./build.sh --native --static --no-dec --release --enable-lto
```

Consider that you may want to opt for using the Clang compiler on Linux instead of GCC. This is recommended for much faster build times. You can do this by running `export CC=clang CXX=clang++` before running the build script, provided you have Clang installed & in your PATH.

If you'd like to build with Dolby Vision support, you can do so by adding `--enable-libdovi` to the build script's args. This assumes you have libdovi installed on your system already, which is a separate process related to [`dovi_tool`](https://github.com/quietvoid/dovi_tool/blob/main/dolby_vision/README.md#libdovi-c-api).

3. The compiled binaries will be located in `Bin/Release` if you navigate back to the root directory:

```bash
cd ../../Bin/Release/ # navigate quickly to Bin/Release from build dir
./SvtAv1EncApp --version
```

4. \[Optional\] On most Linux & macOS machines, you can install the compiled binary by running `sudo cp SvtAv1EncApp /usr/local/bin` if you'd like to have it available system-wide.

```bash
sudo cp SvtAv1EncApp /usr/local/bin
```

Now, you are all set! You can encode with the `SvtAv1EncApp` binary. Happy encoding!

#### CMake

0. Ensure you have the necessary dependencies for this process, which may include:
    - Git
    - CMake 3.23 or newer
    - Yasm 1.2.0 or newer
    - GCC or Clang (preferably Clang)

1. Clone the repository & create a build directory:

```bash
git clone https://github.com/gianni-rosato/svt-av1-psy
cd svt-av1-psy && mkdir svt_build && cd svt_build
```

2. Run the CMake installation command:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_DEC=OFF -DSVT_AV1_LTO=ON -DNATIVE=ON -DCMAKE_CXX_FLAGS="-O3" -DCMAKE_C_FLAGS="-O3" -DCMAKE_LD_FLAGS="-O3" && make -j$(nproc)
```

3. Navigate to the `Bin/Release` directory to find the compiled binary:

```bash
cd ../Bin/Release/
rm -rf ../../svt_build
./SvtAv1EncApp --version
```

4. \[Optional\] On most Linux & macOS machines, you can install the compiled binary by running `sudo cp SvtAv1EncApp /usr/local/bin` if you'd like to have it available system-wide.

```bash
sudo cp SvtAv1EncApp /usr/local/bin
```

That's all! If you'd like to build with Dolby Vision support, you can do so by adding `-DLIBDOVI_FOUND=1` to the CMake command. This assumes you have libdovi installed on your system already, which is a separate process related to [`dovi_tool`](https://github.com/quietvoid/dovi_tool/blob/main/dolby_vision/README.md#libdovi-c-api).

### Windows

On Windows, you can choose to build SVT-AV1-PSY using the provided batch script or with msys2. Both methods for building the SVT-AV1-PSY standalone binary are detailed below.

#### Batch Script

On Windows, a standalone binary can be built with the provided batch script in `Build/windows/`. More information about this method can be found in the more verbose [SVT-AV1 build guide](Build-Guide.md).

#### MSYS2

MSYS2 is the best option for building in Windows, as it provides a Unix-like environment for building SVT-AV1-PSY. This makes the compilation procedure the same as described above for Linux & macOS. The full process is detailed below:

0. Make sure you have downloaded & installed MSYS2 from [the MSYS2 website](https://www.msys2.org/) before beginning the build process.

1. Start the UCRT64 console & install the required dependencies:
```bash
pacman -Syu --needed git mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-yasm
```

2. \[Optional\] Clang is the recommended compiler for SVT-AV1 & SVT-AV1-PSY, so you may download that with the following command:

```bash
pacman -Syu --needed mingw-w64-ucrt-x86_64-clang
```

2. Follow the steps at [Bash Script](#bash-script) or [CMake](#cmake) section, and you will be all set! Please note that CMake may require you to include `-G "Ninja"` in the command.

## Project Development

SVT-AV1-PSY is a project that aims to enhance the Scalable Video Technology for AV1 Encoder with perceptual enhancements for psychovisually optimal AV1 encoding. The ultimate goal is to create *the best encoding implementation for perceptual quality with AV1*. The development of this project involves a collaborative effort from a team of dedicated developers and contributors who are committed to improving the perceptual quality of AV1 encoding.

The development process involves rigorous community testing and optimization to ensure that the encoder and decoder deliver the best possible performance. The team uses a variety of tools and methodologies to analyze and improve the performance of the encoder and decoder, including subjective analyses. SSIMULACRA2 and XPSNR are used extensively for metrics testing, and the team is committed to improving the overall quality and performance of the encoder using these two metrics as general guidelines and benchmarks. However, our goal is not to improve metric scores but to improve the overall perceptual quality of the encoder; that will always come first, so naturally some changes may be made that degrade metric performance in favor of perceptual fidelity per bit.

## Self-Testing

If you wish to test local changes to see if you have improved on the perceptual quality of the encoder, you can use SSIMULACRA2 & XPSNR locally on legal content. This isn't a requirement for opening a PR, but it helps us quite a bit.

More about SSIMULACRA2:
- https://github.com/cloudinary/ssimulacra2/blob/main/README.md

More about XPSNR:
- http://www.ecodis.de/xpsnr.htm
- https://www.itu.int/pub/S-JOURNAL-ICTS.V3I1-2020-8

SSIMULACRA2 implementations:
- https://github.com/rust-av/ssimulacra2_bin (Standalone, slower)
- https://github.com/dnjulek/vapoursynth-ssimulacra2 (Vapoursynth, faster)

XPSNR implementations:
- https://github.com/fraunhoferhhi/xpsnr (FFmpeg 7.0, very fast)
- https://github.com/gianni-rosato/xpsnr (FFmpeg 6.0, very fast)

With the exception of vanilla VMAF, PSNR, or SSIM, if you believe other metrics are relevant, you may include them in your analysis if you also include results from either SSIMULACRA2 or XPSNR in addition. VMAF NEG is accepted. Single frame visual comparisons are helpful as well, though they cannot be relied on by themselves.

## Project Goals

 As stated above, the primary goal of SVT-AV1-PSY is to create the best encoding implementation for perceptual quality with AV1. This involves optimizing the encoder and decoder to deliver superior psychovisual performance. Psychovisual optimizations aim to improve the perceived visual fidelity of encoded video. Fidelity is distinct from mathematical loss or appeal in that it is a measure of how well the encoded video matches the original source video in terms of visual quality according to our eyes. This is a complex and subjective task compared to simply improving the BD-rate of certain metrics, but the team is committed to improving this through rigorous testing and optimization.

## Branches: Testing vs Master

In the SVT-AV1-PSY project, there are two main branches: the "testing" branch and the "master" branch.

- The **"testing" branch** is where new (potentially broken) features and improvements are initially introduced. This branch is used for development and testing purposes, allowing the team to evaluate the impact of changes and ensure they work as expected before they are merged into the master branch.
- The **"master" branch** is the stable branch of the project. It contains the tested and approved changes from the testing branch. This is the branch that users should use for the most stable and reliable performance.

By maintaining these two branches, the SVT-AV1-PSY project ensures a balance between innovation through the testing branch and stability through the master branch.

## Which Branch Do I Use?

If unsure, **please use the "master" branch**.

The "master" branch represents the stable state of the project, containing only those changes that have been thoroughly tested and approved. Therefore, for regular use, the "master" branch is recommended as it generally provides more reliable and visually superior performance.

While the "testing" branch is an essential part of the development process, it is not recommended for regular use. The primary reason for this is that the "testing" branch is a work-in-progress area where new features and improvements are being introduced and tested. As a result, it may not perform as well visually as the "master" branch, may not compile, or may produce a binary that doesn't work properly.

The "testing" branch is subject to frequent changes and updates, which can introduce instability and unpredictability. These changes can affect the visual performance of the encoder and decoder, leading to suboptimal results.

We do not accept issue reports coming from the testing branch. If you encounter an issue while using the "testing" branch, please wait for the changes to be merged into the "master" branch and then test again. If the issue persists, please report it.

## Contact Us

If you have any questions or need assistance, please feel free to reach out to us. You can contact us through the [AV1 for Dummies](https://discord.gg/bbQD5MjDr3) Discord server, where you can find the SVT-AV1-PSY channel.

Alternatively, you can reach out to us via the GitHub Issues here if you have a valid issue to report. Finally, you can contact Gianni directly on Matrix at `@computerbustr:matrix.org` if you have a specific query that the other two methods cannot properly address.

## Documentation

**Guides**
- [System Requirements](System-Requirements.md)
- [How to run SVT-AV1 within ffmpeg](Ffmpeg.md)
- [Standalone Encoder Usage](svt-av1_encoder_user_guide.md)
- [Decoder Usage](svt-av1_decoder_user_guide.md)
- [List of All Parameters](Parameters.md)
- [Build Guide](Build-Guide.md)
- [ARM Build Guide](ARM-Build-Guide.md)

**Common Questions/Issues**
- [What presets do](CommonQuestions.md#what-presets-do)
- [Scene change detection](CommonQuestions.md#scene-change-detection)
- [GOP size selection](CommonQuestions.md#gop-size-selection)
- [Threading and efficiency](CommonQuestions.md#threading-and-efficiency)
- [Practical advice about grain synthesis](CommonQuestions.md#practical-advice-about-grain-synthesis)
- [Improving decoding performance](CommonQuestions.md#improving-decoding-performance)
- [Tuning for animation](CommonQuestions.md#tuning-for-animation)
- [8 vs. 10-bit encoding](CommonQuestions.md#8-or-10-bit-encoding)
- [HDR and SDR video](CommonQuestions.md#hdr-and-sdr)
- [Options that give the best encoding bang-for-buck](CommonQuestions.md#options-that-give-the-best-encoding-bang-for-buck)
- [Multi-pass encoding](CommonQuestions.md#multi-pass-encoding)
- [CBR, VBR, and CRF modes](CommonQuestions.md#bitrate-control-modes)

**Presentations**
- [Big Apple Video 2019](https://www.youtube.com/watch?v=lXqOaYNo8m0)
- [Video @ Scale 2021](https://atscaleconference.com/videos/highly-efficient-svt-av1-based-solutions-for-vod-applications/?contact-form-id=124119&contact-form-sent=163268&contact-form-hash=d4bb3fd420fae91cd39c11bdb69f970a05a152a9&_wpnonce=bba8096d24#contact-form-124119)

**Papers and Blogs**
- [Netflix Blog 2020](https://netflixtechblog.com/svt-av1-an-open-source-av1-encoder-and-decoder-ad295d9b5ca2)
- [SPIE 2020](https://www.spiedigitallibrary.org/conference-proceedings-of-spie/11510/1151021/The-SVT-AV1-encoder--overview-features-and-speed-quality/10.1117/12.2569270.full)
- [SPIE 2021](https://www.spiedigitallibrary.org/conference-proceedings-of-spie/11842/118420T/Towards-much-better-SVT-AV1-quality-cycles-tradeoffs-for-VOD/10.1117/12.2595598.full)
- [SVT-AV1 - Tech Blog 2022](https://networkbuilders.intel.com/blog/svt-av1-enables-highly-efficient-large-scale-video-on-demand-vod-services)
- [SPIE 2022](https://www.spiedigitallibrary.org/conference-proceedings-of-spie/12226/122260S/Enhancing-SVT-AV1-with-LCEVC-to-improve-quality-cycles-trade/10.1117/12.2633882.full)
- [Adaptive Steaming Common Test Conditions](https://aomedia.org/docs/SIWG-D001o.pdf)
- [ICIP 2023](https://arxiv.org/abs/2307.05208)

**Design Documents**
- [Encoder Design](svt-av1-encoder-design.md)
- [Decoder Design](svt-av1-decoder-design.md)

**Technical Appendices**
- [Why build with LTO?] (CommonQuestions.md#why-build-with-lto)
- [Adaptive Prediction Structure Appendix](Appendix-Adaptive-Prediction-Structure.md)
- [Altref and Overlay Pictures Appendix](Appendix-Alt-Refs.md)
- [CDEF Appendix](Appendix-CDEF.md)
- [CfL Appendix](Appendix-CfL.md)
- [Compliant Subpel Interpolation Filter Search Appendix](Appendix-Compliant-Subpel-Interpolation-Filter-Search.md)
- [Compound Mode Prediction Appendix](Appendix-Compound-Mode-Prediction.md)
- [Deblocking Loop Filter (LF) Appendix](Appendix-DLF.md)
- [Film Grain Synthesis](Appendix-Film-Grain-Synthesis.md)
- [Global Motion Appendix](Appendix-Global-Motion.md)
- [Intra Block Copy Appendix](Appendix-Intra-Block-Copy.md)
- [IPP Pass Appendix](Appendix-IPP-Pass.md)
- [Local Warped Motion appendix](Appendix-Local-Warped-Motion.md)
- [Mode Decision Appendix](Appendix-Mode-Decision.md)
- [Motion Estimation Appendix](Appendix-Open-Loop-Motion-Estimation.md)
- [Overlapped Block Motion Compensation Appendix](Appendix-Overlapped-Block-Motion-Compensation.md)
- [Palette Prediction Appendix](Appendix-Palette-Prediction.md)
- [Rate Control Appendix](Appendix-Rate-Control.md)
- [Recursive Intra Appendix](Appendix-Recursive-Intra.md)
- [Restoration Filter Appendix](Appendix-Restoration-Filter.md)
- [SQ Weight Appendix](Appendix-SQ-Weight.md)
- [Super-resolution Appendix](Appendix-Super-Resolution.md)
- [Temporal Dependency Model](Appendix-TPL.md)
- [Transform Search Appendix](Appendix-TX-Search.md)
- [Reference Scaling Appendix](Appendix-Reference-Scaling.md)
- [Variance Boost Appendix](Appendix-Variance-Boost.md)
