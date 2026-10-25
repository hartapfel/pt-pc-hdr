# P.T. PC Port — Native HDR fork

An independent HDR fork of [LoreanXavier/pt-pc](https://github.com/LoreanXavier/pt-pc), based on official **v1.0.8** (`3b1392abe5215773d4c353f5bc1ecb85233be68a`). Original port credit and its MIT license remain with LoreanXavier; P.T. belongs to Konami.

## Install

Download the **[Windows installer](https://github.com/hartapfel/pt-pc-hdr/releases/latest)** from this fork's releases. A portable Windows ZIP and source archives are also provided. The installer includes the extraction helper and its LGPL corresponding source. Supply your own supported P.T. package or extracted game files, following the [original installation requirements](docs/installer.md). No original game archives, keys, saves or generated scene previews are included.

Enable HDR on your OS and the game's HDR output, then set HDR peak brightness, HDR game brightness and HDR UI brightness for your display. Existing settings are preserved; select High or Ultra to apply the new effect defaults. Report fork-specific issues [here](https://github.com/hartapfel/pt-pc-hdr/issues).

## Latest HDR revision: 1.0.8.1-hdr

- Native HDR color-grading controls, wide-gamut P3-D65 compression and independent UI brightness.
- Original defaults to neutral grading with wide gamut On; HDR look sets saturation to 150% and highlights to 145%. Other combinations show Custom.
- Exposure, contrast, highlights, shadows, saturation, highlight saturation and Blowout controls; 100% Blowout is grayscale.
- See-through settings preview across Graphics, including inactive sliders under Path Tracing; a toggle restores the opaque menu.
- Clearer HDR calibration labels and ordering; the SDR brightness screen cannot change HDR grading.
- SDR and HDR share the same smooth shadow curve. SDR retains its original brightness slider; HDR uses neutral scene response independently of that slider.

Existing settings are preserved. HDR fork releases use `<upstream version>.<fork revision>-hdr`; the initial `1.0.8-hdr` is revision zero. Updates come from this fork and retain its signing key. Each release presents one Windows installer labeled with its current version. Its download filename remains `P.T.PC.Port.1.0.8-HDR.Setup.exe` because existing clients require it; the installed version follows the release tag.

## Changes from official 1.0.8

- Preserve scene HDR through lighting, materials, reflections, path tracing and post effects; remove unnecessary SDR ceilings and use floating-point intermediates.
- Replace the legacy HDR boost with color-preserving highlight roll-off toward configured peak brightness, paper-white controls and selectable SDR EOTF emulation (gamma 2.2 by default).
- Improve shadow transitions and HDR color grading with a 64³ float LUT, tetrahedral interpolation and highlight-range reconstruction. Preserve the authored BT.709 colors; no artificial gamut expansion.
- Rework bloom, lens flares and ghosts for linear-light HDR, smoother filtering and independent intensity controls.
- Add selectable original or improved perceptual film grain, and Lilium HDR RCAS sharpening.
- Stabilize eye adaptation with robust HDR metering and frame-time-based exposure transitions.
- Improve lens distortion/chromatic-aberration reconstruction while preserving the original channel mapping; improve motion blur's temporal scaling, velocity precision and depth rejection; replace depth of field with layered, coverage-aware filtering.
- Correct effect ordering, temporal history and HDR output composition, including the frame-generation output path.
- Simplify the effect menu. Original (PS4) restores vanilla effect strengths and grain. High/Ultra use bloom 100%, flare 50%, ghosts 25%, improved grain 50%.

See [HDR implementation](docs/hdr-native.md), [post-processing](docs/hdr-postprocess.md), [optics](docs/hdr9-optics.md), and [build/release instructions](docs/hdr-build.md). HDR1–HDR9 in technical documents are internal development revisions; the current public version is **1.0.8.1-hdr**. See [HDR color grading](docs/hdr-color-grading.md) for the new controls and validation.

## Build and attribution

The fork retains upstream's CMake/Ninja/Clang build and native installer tooling. See [rebuilding the release](docs/hdr-build.md) for prerequisites, pinned dependencies, extraction-helper source and signing. Windows x64 is the tested release platform; no untested Linux/macOS binaries are published. Build outputs are not promised to be byte-identical across compiler/SDK versions or signing keys.

The original author's [README](README.upstream.md), [documentation](docs), [LICENSE](LICENSE) and third-party notices remain available. Shader references and adapted algorithms are credited in [HDR post-processing licenses](docs/hdr-post-licenses.txt). This fork preserves the original look and credits while providing optional presentation improvements.
