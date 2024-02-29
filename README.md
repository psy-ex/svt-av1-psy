# SVT-AV1-PSY

SVT-AV1-PSY is the Scalable Video Technology for AV1 (SVT-AV1 Encoder and Decoder) with perceptual enhancements for psychovisually optimal AV1 encoding. The goal is to create the best encoding implementation for perceptual quality with AV1.

### Feature Additions

- `--variance-boost-strength` *1 to 4*

Provides control over our augmented AQ Modes 0 and 2 which can utilize variance information in each frame for more consistent quality under high/low contrast scenes. Four curve options are provided, and the default is curve 2. 1: mild, 2: gentle, 3: medium, 4: aggressive

- `--variance-octile` *1 to 8*

Controls how "selective" the algorithm is when boosting superblocks, based on their low/high 8x8 variance ratio. A value of 1 is the least selective, and will readily boost a superblock if only 1/8th of the superblock is low variance. Conversely, a value of 8 will only boost if the *entire* superblock is low variance. Lower values increase bitrate. The default value is 6.

- `--enable-alt-curve` *0 and 1*

Enable an alternative variance boost curve, with different bit allocation and visual characteristics. The default is 0.

- `Presets -2 & -3`

Terrifically slow encoding modes for research purposes.

- `Tune 3`

A new tune based on Tune 2 (SSIM) called SSIM with Subjective Quality Tuning. Generally harms metric performance in exchange for better visual fidelity.

- `--sharpness` *-7 to 7*

A parameter for modifying loopfilter deblock sharpness and rate distortion to improve visual fidelity. The default is 0 (no sharpness).

- `--dolby-vision-rpu` *path to file*

Set the path to a Dolby Vision RPU for encoding Dolby Vision video. SVT-AV1-PSY needs to be built with the `enable-libdovi` flag enabled in build.sh (see `./Build/linux/build.sh --help` for more info) (Thank you @quietvoid !)

- `Progress 3`

A new progress mode that provides more detailed information about the encoding process.

- `--fgs-table` *path to file* (**[Merged to Mainline](https://gitlab.com/AOMediaCodec/SVT-AV1/-/commit/ae7ce1abc5f3f7913624f728ae123f8b8c1e30de)**)

Argument for providing a film grain table for synthetic film grain (similar to aomenc's '--film-grain-table=' argument).

### Modified Defaults

SVT-AV1-PSY has different defaults than mainline SVT-AV1 in order to provide better visual fidelity out of the box. They include:

- Default 10-bit color depth when given a 10-bit input.
- Disable film grain denoising by default, as it often harms visual fidelity.
- Default to Tune 2 instead of Tune 1, as it reliably outperforms Tune 1 in our metrics of choice.
- Enable quantization matrices by default.
- Set minimum QM level to 0 by default.

*We are not in any way affiliated with the Alliance for Open Media or any upstream SVT-AV1 project contributors who have not also contributed here.*

### Other Changes

- `--color-help`
Prints the information found in Appendix A.2 of the user guide in order to help users more easily understand the Color Description Options in SvtAv1EncApp.

# Getting Involved

For more information on SVT-AV1-PSY and this project's mission, see the [PSY Development](Docs/PSY-Development.md) page.

## License

Up to v0.8.7, SVT-AV1 is licensed under the BSD-2-clause license and the
Alliance for Open Media Patent License 1.0. See [LICENSE](LICENSE-BSD2.md) and
[PATENTS](PATENTS.md) for details. Starting from v0.9, SVT-AV1 is licensed
under the BSD-3-clause clear license and the Alliance for Open Media Patent
License 1.0. See [LICENSE](LICENSE.md) and [PATENTS](PATENTS.md) for details.

SVT-AV1-PSY does not feature license modifications from mainline SVT-AV1.

## Documentation

Keep in mind that these documents are not necessarily up-to-date with the most recent changes in SVT-AV1-PSY. They are a good reference for general usage and understanding of the encoder.

**Guides**
- [System Requirements](Docs/System-Requirements.md)
- [How to run SVT-AV1 within ffmpeg](Docs/Ffmpeg.md)
- [Standalone Encoder Usage](Docs/svt-av1_encoder_user_guide.md)
- [List of All Parameters](Docs/Parameters.md)
- [Build Guide](Docs/Build-Guide.md)
- [ARM Build Guide](Docs/ARM-Build-Guide.md)

**Common Questions/Issues**
- [Why build with LTO?](Docs/CommonQuestions.md#why-build-with-lto)
- [Why build with PGO?](Docs/CommonQuestions.md#why-build-with-pgo)
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
- [SPIE 2024](https://www.spiedigitallibrary.org/conference-proceedings-of-spie/13137/131370W/Benchmarking-hardware-and-software-encoder-quality-and-performance/10.1117/12.3031754.full)
- [ICIP 2024](https://aomedia.org/docs/Software_Implementation_Working_Group_Update_ICIP2024.pdf)

**Design Documents**
- [Encoder Design](Docs/svt-av1-encoder-design.md)

**Technical Appendices**
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

**How Can I Contribute?**
- [SVT-AV1 Contribution Guide](Docs/Contribute.md)
