# Native HDR pipeline and controls

The HDR path preserves scene radiance through lighting, reflections, post
processing and the authored grade before display mapping. It replaces the
previous auxiliary highlight-gain texture, which tried to restore highlight
brightness after the SDR image had already clipped. Alpha remains coverage.

## Pipeline and color domains

1. Preserve scene-linear radiance in float lighting, material, reflection and
   bloom resources. Remove SDR ceilings only when native HDR is active; keep
   normalized coverage, material IDs and finite-storage guards bounded.
2. Combine HDR bloom and flares as radiance. Preserve the original shadow toe;
   it is an authored operation on encoded values, not a substitute for sRGB.
3. Keep the upstream DOF kernels, motion timing/taps, flare profile, distortion
   mapping and bloom filter. HDR optical filtering decodes each texel before
   interpolation and re-encodes the result for the existing post-chain domain.
   Motion blur uses linear scratch color and composites coverage in linear
   light in HDR. Its SDR kernels/order remain upstream's. HDR motion blur runs
   before the creative LUT; upstream's SDR street placement is retained.
4. Build a 64-cube float grade and use tetrahedral interpolation. The HDR LUT
   bridge compresses values above linear 0.18 into an interior max-channel
   proxy, grades there, and restores the scale. It avoids sampling clipped
   white and reconstructs an identity grade without discarding highlights.
5. Apply HDR exposure, contrast, highlights/shadows and chroma controls before
   the peak-dependent shoulder and gamut compression. Wide gamut targets
   P3-D65; it preserves valid authored colors rather than stretching BT.709
   primaries. Blowout blends the mapped linear color toward luminance gray.
6. HDR-aware RCAS runs after scene display mapping; grain and UI follow. UI
   brightness is independent of scene white and grading. Convert once to the
   selected SDR reference response, then output linear scRGB or BT.2020/PQ.
   Frame-generation UI extraction uses the same output conversion and retains
   signed wide-gamut contributions.

The public HDR switch still defaults to Off. The original SDR brightness
slider remains effective only in SDR. HDR uses neutral scene response, with
separate scene-white and UI-white controls. SDR EOTF emulation is selectable
sRGB, Gamma 2.2 (default), or Gamma 2.4; this describes the SDR display response
being emulated, not the HDR output encoding.

## Native menu and settings

- Calibration order: peak brightness, HDR game brightness, HDR UI brightness.
- Original grading: neutral adjustments and wide gamut On. HDR look: saturation
  150%, highlights 145%, wide gamut On. Any other combination reads Custom.
- Exposure: -4 to +4 EV. Percentage grading: 0 to 200%; Blowout: 0 to 100%,
  with grayscale at 100%.
- Live preview while adjusting HDR and throughout the Graphics page, including
  disabled rows; its toggle restores the opaque menu. Dialogs retain their
  backdrops and input behavior.
- Bloom, flare and ghost intensity controls plus selectable original/perceptual
  film grain. Original grain remains the default. Existing graphics presets
  retain their original strength, grain, ghost and quality values. Legacy
  bloom/ghost switches migrate to equivalent intensities.

The exposure improvements operate in both SDR and HDR: luminance is reduced
robustly, the sampled frame's pre-exposure is removed before deriving the
absolute target EV, and adaptation uses elapsed time. The higher-precision LUT
also applies to both modes. These are intentional shared improvements; the PR
does not claim the whole SDR image is pixel-identical to upstream.

## Scope and review map

- `radiance.glsl`, material/reflection/PT shaders and `scene_renderer.cpp`:
  preserve range, resource formats, LUT generation and exposure.
- `tonemap.frag`, `post_filter.glsl`, DOF/motion/screen shaders and
  `scene_post.cpp`: HDR-compatible original post effects and order.
- `hdr_color.glsl`, `color_gamut.glsl`, `composite_output.glsl`, renderer/UI:
  grading, output conversion, independent UI and frame-generation composition.
- `hdr_color_settings.h`, settings and native menu code: persistence, presets,
  controls and preview behavior.

The fork's optical redesigns, custom smooth shadow curve, revised graphics
preset values, release version, update feed/key, installer branding, build
preset changes and chronological development notes are not included. Upstream
README, funding information, CI and release packaging policy remain intact.
The package includes the notices for adapted grain and RCAS code.

## Validation

Build the existing release preset with the local compiler/SDK configuration.
Targets: `pt_platform_test`, `pt_settings_roundtrip_test`,
`pt_graphics_preset_test`, `pt_exposure_test`, `pt_hdr_output_test`,
`pt_composite_output_test`, `pt_d3d12_hdr_framegen_test`,
`pt_pc_settings_refresh_test`, `pt_hdr_pipeline_test`,
`pt_d3d12_backend_test`, `pt_language_default_test`, `pt_reflection_mix_test`.
Pass a temporary directory to settings-roundtrip, a temporary INI path to
preset tests, and `--out <temporary directory>` to the backend test.

The backend tests execute production SPIR-V and DXIL: scene and bloom range,
LUT reconstruction, vanilla shadow parity, original lens coordinates, DOF and
motion radiance preservation. Composite tests cover output encodings, peak
levels, reference EOTFs, grading extremes, UI and frame-generation extraction.
Settings tests cover missing legacy keys and unchanged graphics-preset values.
The legacy Ultra test fixture now edits the actual INI key rather than a
comment containing the same text; migration behavior itself is unchanged.

`tests/pad/hdr_color_menu.txt` exercises the native menus with isolated saves
and `PT_HDR_HEADLESS=scrgb` or `hdr10`; see its header for invocation details.
Windows x64 Clang Release builds with OpenXR both enabled and disabled passed.
Vulkan/D3D12 GPU tests, 53 native HDR menu expectations per backend, SDR
graphics preview and HDR scenes with DOF/motion blur enabled were exercised.
Physical-display measurements, a full playthrough and live generated-frame
or OpenXR-headset gameplay are not certified by these checks.

Float intermediates increase render-target memory and bandwidth. The two
64-cube RGBA32F LUTs occupy 8 MiB together. Performance across GPUs and
non-Windows builds remains to be checked by the upstream project.

## References

- [ICC sRGB definition](https://registry.color.org/rgb-registry/srgb): the
  piecewise transfer function used at explicitly identified encode/decode
  boundaries. The original shadow toe is retained separately.
- [Microsoft Advanced Color](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range):
  extended float color, linear composition and scRGB output conventions.
- [AMD FidelityFX FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR): RCAS
  basis; HDR luminance adaptation follows Lilium's adaptation via RenoDX.
- [RenoDX](https://github.com/clshortfuse/renodx): perceptual grain and reference
  HDR grading patterns. Adapted code notices are in `hdr-post-licenses.txt`.
