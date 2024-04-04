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

- `Extended CRF`

Provides a more versatile and granular way to set CRF. Range has been expanded to 70 (from 63) to help with ultra-low bitrate encodes, and can now be set in quarter-step (0.25) increments.

- `--qp-scale-compress-strength` *0 to 3*

Increases video quality temporal consistency, especially with clips that contain film grain and/or contain fast-moving objects.

- `--enable-dlf 2`

Enables a more accurate loop filter that prevents blocking, for a modest increase in compute time (most noticeable at presets 7 to 9)

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

# Building

For Linux, macOS, & Windows build instructions, see the [PSY Development](Docs/PSY-Development.md) page.

# Getting Involved

For more information on SVT-AV1-PSY and this project's mission, see the [PSY Development](Docs/PSY-Development.md) page.

One way to get involved is to use SVT-AV1-PSY in your own AV1 encoding projects, increasing the impact our work has on others! You and your users will also be able to provide feedback on the encoder's overall performance and report any issues you encounter. Your name will also be added to this page.

**Projects Featuring SVT-AV1-PSY:**

- [Aviator](https://github.com/gianni-rosato/aviator) ~ an AV1 encoding GUI by @gianni-rosato
- [rAV1ator CLI](https://github.com/gianni-rosato/rav1ator-cli) ~ a TUI for video encoding with Av1an by @gianni-rosato
- [SVT-AV1-PSY on the AUR](https://aur.archlinux.org/packages/svt-av1-psy-git) ~ by @BlueSwordM
- [SVT-AV1-PSY in CachyOS](https://github.com/CachyOS/CachyOS-PKGBUILDS/pull/144) ~ by @BlueSwordM

## License

Up to v0.8.7, SVT-AV1 is licensed under the BSD-2-clause license and the
Alliance for Open Media Patent License 1.0. See [LICENSE](LICENSE-BSD2.md) and
[PATENTS](PATENTS.md) for details. Starting from v0.9, SVT-AV1 is licensed
under the BSD-3-clause clear license and the Alliance for Open Media Patent
License 1.0. See [LICENSE](LICENSE.md) and [PATENTS](PATENTS.md) for details.

SVT-AV1-PSY does not feature license modifications from mainline SVT-AV1.

## Documentation

For additional docs, see the [PSY Development](Docs/PSY-Development.md) page.
