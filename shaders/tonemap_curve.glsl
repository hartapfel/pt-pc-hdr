// Shared SDR/HDR shadow treatment. hdr_output.z selects toe quality,
// independently of hdr_output.x (output range). Do not apply the toe twice.
vec3 ShadowToe(vec3 x, bool smooth_toe) {
    const float knee = 0.04419417382415922; // sqrt(1/512), in encoded space.
    vec3 original = x*x / max(vec3(knee), x);
    if (!smooth_toe) return original;
    vec3 t = clamp((x - 0.5*knee) / knee, 0.0, 1.0);
    vec3 transition = x - 0.25*knee*(1.0-t*t*(3.0-2.0*t));
    return mix(original, transition, greaterThan(x, vec3(0.5*knee)));
}

vec3 ScreenTonemap(vec3 x, vec3 bloom) {
    vec3 x2 = x * x;
    vec3 r = clamp(x2 + bloom - x2 * bloom, 0.0, 1.0);
    return ShadowToe(sqrt(r), frame.hdr_output.z > 0.5);
}

// HDR bloom is additive radiance before grading and display mapping. The old
// screen blend dimmed highlights, then changed slope separately in R/G/B at .5.
vec3 ScreenTonemapHdr(vec3 x, vec3 bloom) {
    vec3 encoded = SrgbEncode(SrgbDecode(max(x, vec3(0.0))) + max(bloom, vec3(0.0)));
    return ShadowToe(encoded, frame.hdr_output.z > 0.5);
}
