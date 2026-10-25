#version 460
#include "hdr_shoulder.glsl"
#include "composite_output.glsl"
#include "film_grain.glsl"
#include "hdr_rcas.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D grain_noise;
layout(set = 0, binding = 2) uniform sampler2D ui_top;

layout(push_constant) uniform CompositeParams {
    float sdr_eotf;
    float brightness;
    float mode;
    float pad;
    vec4 fade;
    vec4 grain;
    vec4 grain_offset;
    vec4 hdr;
} params;

vec3 MappedScene(vec2 uv) {
    vec3 color = texture(scene_color, uv).rgb;
    if (params.pad > 0.5 && params.pad < 1.5) {
        color = vec3(dot(color, vec3(0.2126,0.7152,0.0722)));
    } else if (params.pad < 2.5 && params.pad > 1.5) {
        color *= vec3(1.06,1.0,0.92);
    } else if (params.pad < 3.5 && params.pad > 2.5) {
        color *= vec3(0.92,1.0,1.06);
    } else if (params.pad > 3.5) {
        color = mix(vec3(dot(color,vec3(0.2126,0.7152,0.0722))),color,0.35);
    }
    if (params.hdr.z > 0.5) {
        // Map scene highlights before the UI overlays. Leave every SDR color
        // unchanged, and scale RGB together to preserve highlight chromaticity.
        vec3 linear_color = DecodeSdrReference(HdrSceneBrightness(color, params.brightness), params.sdr_eotf);
        float peak = max(linear_color.r, max(linear_color.g, linear_color.b));
        if (peak > 0.0) linear_color *= HdrShoulder(peak, max(params.hdr.y / params.hdr.x, 1.0)) / peak;
        color = EncodeSdrReference(linear_color, params.sdr_eotf);
    }
    return color;
}

void main() {
    vec4 sampled = texture(scene_color, in_uv);
    vec3 color = sampled.rgb;
    if (params.mode > 0.5) {
        vec4 top = params.hdr.w > 0.5 ? texture(ui_top, in_uv) : vec4(0.0);
        if (params.grain.x > 0.0) {
            float peak = params.hdr.z > 0.5 ? max(params.hdr.y / params.hdr.x, 1.0) : 1.0;
            color = ApplyFilmGrain(color, in_uv, gl_FragCoord.xy, params.grain,
                params.grain_offset, grain_noise, params.sdr_eotf, peak);
        }
        color = color * (1.0 - top.a) + top.rgb;
        out_color = vec4(CompositeDisplayColor(color, params.brightness, params.grain_offset.w, params.hdr.xy, params.sdr_eotf), 1.0);
        return;
    }
    color = MappedScene(in_uv);
    if (params.grain.w > 0.0) {
        vec2 texel = 1.0 / vec2(textureSize(scene_color, 0));
        float peak = params.hdr.z > 0.5 ? max(params.hdr.y / params.hdr.x, 1.0) : 1.0;
        color = EncodeSdrReference(HdrRcas(DecodeSdrReference(color, params.sdr_eotf),
            DecodeSdrReference(MappedScene(in_uv + vec2(0, -texel.y)), params.sdr_eotf),
            DecodeSdrReference(MappedScene(in_uv + vec2(-texel.x, 0)), params.sdr_eotf),
            DecodeSdrReference(MappedScene(in_uv + vec2(texel.x, 0)), params.sdr_eotf),
            DecodeSdrReference(MappedScene(in_uv + vec2(0, texel.y)), params.sdr_eotf),
            params.grain.w, peak), params.sdr_eotf);
    }
    out_color = vec4(mix(color, params.fade.rgb, params.fade.a), 1.0);
}
