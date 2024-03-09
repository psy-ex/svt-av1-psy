[Top level](../README.md)

# SVT-AV1-PSY: Additional Info

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
- https://github.com/fraunhoferhhi/xpsnr (FFmpeg 4.4, very fast)
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
- [System Requirements](Docs/System-Requirements.md)
- [How to run SVT-AV1 within ffmpeg](Docs/Ffmpeg.md)
- [Standalone Encoder Usage](Docs/svt-av1_encoder_user_guide.md)
- [Decoder Usage](Docs/svt-av1_decoder_user_guide.md)
- [List of All Parameters](Docs/Parameters.md)
- [Build Guide](Docs/Build-Guide.md)
- [ARM Build Guide](Docs/ARM-Build-Guide.md)

**Common Questions/Issues**
- [What presets do](Docs/CommonQuestions.md#what-presets-do)
- [Scene change detection](Docs/CommonQuestions.md#scene-change-detection)
- [GOP size selection](Docs/CommonQuestions.md#gop-size-selection)
- [Threading and efficiency](Docs/CommonQuestions.md#threading-and-efficiency)
- [Practical advice about grain synthesis](Docs/CommonQuestions.md#practical-advice-about-grain-synthesis)
- [Improving decoding performance](Docs/CommonQuestions.md#improving-decoding-performance)
- [Tuning for animation](Docs/CommonQuestions.md#tuning-for-animation)
- [8 vs. 10-bit encoding](Docs/CommonQuestions.md#8-or-10-bit-encoding)
- [HDR and SDR video](Docs/CommonQuestions.md#hdr-and-sdr)
- [Options that give the best encoding bang-for-buck](Docs/CommonQuestions.md#options-that-give-the-best-encoding-bang-for-buck)
- [Multi-pass encoding](Docs/CommonQuestions.md#multi-pass-encoding)
- [CBR, VBR, and CRF modes](Docs/CommonQuestions.md#bitrate-control-modes)

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
- [Encoder Design](Docs/svt-av1-encoder-design.md)
- [Decoder Design](Docs/svt-av1-decoder-design.md)

**Technical Appendices**
- [Why build with LTO?] (Docs/CommonQuestions.md#why-build-with-lto)
- [Adaptive Prediction Structure Appendix](Docs/Appendix-Adaptive-Prediction-Structure.md)
- [Altref and Overlay Pictures Appendix](Docs/Appendix-Alt-Refs.md)
- [CDEF Appendix](Docs/Appendix-CDEF.md)
- [CfL Appendix](Docs/Appendix-CfL.md)
- [Compliant Subpel Interpolation Filter Search Appendix](Docs/Appendix-Compliant-Subpel-Interpolation-Filter-Search.md)
- [Compound Mode Prediction Appendix](Docs/Appendix-Compound-Mode-Prediction.md)
- [Deblocking Loop Filter (LF) Appendix](Docs/Appendix-DLF.md)
- [Film Grain Synthesis](Docs/Appendix-Film-Grain-Synthesis.md)
- [Global Motion Appendix](Docs/Appendix-Global-Motion.md)
- [Intra Block Copy Appendix](Docs/Appendix-Intra-Block-Copy.md)
- [IPP Pass Appendix](Docs/Appendix-IPP-Pass.md)
- [Local Warped Motion appendix](Docs/Appendix-Local-Warped-Motion.md)
- [Mode Decision Appendix](Docs/Appendix-Mode-Decision.md)
- [Motion Estimation Appendix](Docs/Appendix-Open-Loop-Motion-Estimation.md)
- [Overlapped Block Motion Compensation Appendix](Docs/Appendix-Overlapped-Block-Motion-Compensation.md)
- [Palette Prediction Appendix](Docs/Appendix-Palette-Prediction.md)
- [Rate Control Appendix](Docs/Appendix-Rate-Control.md)
- [Recursive Intra Appendix](Docs/Appendix-Recursive-Intra.md)
- [Restoration Filter Appendix](Docs/Appendix-Restoration-Filter.md)
- [SQ Weight Appendix](Docs/Appendix-SQ-Weight.md)
- [Super-resolution Appendix](Docs/Appendix-Super-Resolution.md)
- [Temporal Dependency Model](Docs/Appendix-TPL.md)
- [Transform Search Appendix](Docs/Appendix-TX-Search.md)
- [Reference Scaling Appendix](Docs/Appendix-Reference-Scaling.md)
- [Variance Boost Appendix](Docs/Appendix-Variance-Boost.md)
