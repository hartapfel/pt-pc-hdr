#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
using glm::vec2;
using glm::vec3;
using glm::vec4;
using glm::mat3;
using glm::all;
using glm::lessThanEqual;
using glm::max;
float min(double a, double b) { return float(std::min(a, b)); }
using glm::abs;
vec3 operator*(vec3 v, double scale) { return v * float(scale); }
vec3 operator*(double scale, vec3 v) { return float(scale) * v; }
vec3 operator+(vec3 v, double scalar) { return v + float(scalar); }
vec3 operator-(vec3 v, double scalar) { return v - float(scalar); }
vec3 operator/(vec3 v, double scalar) { return v / float(scalar); }
float max(float a, double b) { return std::max(a, float(b)); }
float clamp(float v, double lo, double hi) { return std::clamp(v, float(lo), float(hi)); }
vec3 clamp(vec3 v, double lo, double hi) { return glm::clamp(v, float(lo), float(hi)); }
float pow(float v, double power) { return std::pow(v, float(power)); }
vec3 pow(vec3 v, double power) { return glm::pow(v, vec3(float(power))); }
using glm::pow;
using glm::clamp;
#include "../shaders/composite_output.glsl"
}

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    const auto near = [](vec3 a, vec3 b, float tolerance = 1.0e-5f) {
        return glm::all(glm::lessThanEqual(glm::abs(a - b), vec3(tolerance)));
    };
    const vec2 hdr(200, 1000);
    check(near(CompositeDisplayColor(vec3(.2f, .5f, .9f), 1, 0, hdr, 0), vec3(.2f, .5f, .9f)),
          "SDR retains encoded colour");
    check(near(CompositeDisplayColor(vec3(.25f, .0625f, 1), 2, 0, hdr, 0), vec3(.5f, .25f, 1)),
          "SDR brightness remains unchanged");
    check(near(CompositeDisplayColor(vec3(1), 1, 1, hdr, 0), vec3(2.5f)), "scRGB paper white remains 200 nits");
    check(near(CompositeDisplayColor(vec3(1), 1, 2, hdr, 0), vec3(.579133f), 3.0e-5f),
          "HDR10 paper white is PQ encoded at 200 nits");
    check(near(CompositeDisplayColor(SrgbEncode(vec3(5)), 1, 2, hdr, 0), vec3(.751827f), 3.0e-5f),
          "HDR10 preserves extended RGB up to the 1000 nit peak");
    check(near(CompositeDisplayColor(vec3(.02f), 1, 1, hdr, 0), vec3(.02f / 12.92f * 2.5f)),
          "HDR shadows decode the SDR sRGB transfer instead of gamma 2.2");
    bool gamma_matches = true;
    for (float eotf : {0.0f, 1.0f, 2.0f}) {
      for (float brightness : {.5f, 1.0f, 1.5f, 2.0f}) {
        for (int i = 0; i <= 4096; ++i) {
            const float code = float(i) / 4096.0f;
            const float shown = std::pow(code, 1.0f / brightness);
            const float linear = eotf > .5f ? std::pow(shown, eotf > 1.5f ? 2.4f : 2.2f) :
                (shown <= .04045f ? shown / 12.92f : std::pow((shown + .055f) / 1.055f, 2.4f));
            gamma_matches &= near(CompositeDisplayColor(HdrSceneBrightness(vec3(code), brightness), brightness, 1, hdr, eotf), vec3(linear * 2.5f));
            gamma_matches &= near(CompositeDisplayColor(HdrSceneBrightness(vec3(code), brightness), brightness, 2, hdr, eotf),
                                  vec3(PqEncode(linear * 200.0f)), 3.0e-5f);
        }
    }
    }
    check(gamma_matches, "4097-step SDR ramp matches each reference EOTF in scRGB/PQ at four brightness settings");
    check(near(CompositeDisplayColor(vec3(.02f), 1, 1, hdr, 1), vec3(std::pow(.02f, 2.2f) * 2.5f)) &&
          CompositeDisplayColor(vec3(.02f), 1, 1, hdr, 1).r < CompositeDisplayColor(vec3(.02f), 1, 1, hdr, 0).r,
          "gamma 2.2 emulates SDR shadow luminance without the sRGB near-black lift");
    bool round_trip = true;
    for (float linear : {0.0f, .00001f, .001f, .0031307f, .0031308f, .0031309f, .01f, .18f, 1.0f, 5.0f, 100.0f}) {
        round_trip &= near(SrgbDecode(SrgbEncode(vec3(linear))), vec3(linear), std::max(1.0e-7f, linear * 1.0e-6f));
    }
    check(round_trip, "sRGB encode/decode round trip preserves black, transfer-function knee, midgray and extended highlights");
    bool reference_round_trip = true;
    for (float eotf : {0.0f, 1.0f, 2.0f}) {
        for (float linear : {0.0f, .00001f, .001f, .0031308f, .18f, 1.0f, 5.0f, 100.0f}) {
            reference_round_trip &= near(DecodeSdrReference(EncodeSdrReference(vec3(linear), eotf), eotf),
                                         vec3(linear), std::max(1.0e-7f, linear * 1.0e-6f));
        }
        reference_round_trip &= near(CompositeDisplayColor(vec3(.02f, .5f, 1), 1, 0, hdr, eotf), vec3(.02f, .5f, 1));
    }
    check(reference_round_trip, "reference EOTFs round trip extended RGB and never alter SDR output");
    check(near(CompositeDisplayColor(vec3(.4f), .5f, 1, hdr, 1), CompositeDisplayColor(vec3(.4f), 2.f, 1, hdr, 1)),
          "HDR output does not reapply scene brightness to mapped highlights or UI");
    const vec3 red = CompositeDisplayColor(vec3(1, 0, 0), 1, 2, hdr, 0);
    check(near(red, vec3(PqEncode(125.4808f), PqEncode(13.8194f), PqEncode(3.27832f))),
          "HDR10 converts Rec709 primaries to BT2020 before PQ");
    const vec3 base = CompositeDisplayColor(SrgbEncode(vec3(2, .5f, .1f)), 1, 2, hdr, 0);
    bool blends = true, bounded = true;
    for (int r = 0; r <= 10; ++r) {
        for (int g = 0; g <= 10; ++g) {
            for (int b = 0; b <= 10; ++b) {
                const vec3 shown = CompositeDisplayColor(vec3(r, g, b) / 10.0f, 1.3f, 2, hdr, 0);
                const vec4 layer = CompositeUiLayer(shown, base, 1.5f / 1023);
                blends &= near(vec3(layer) + (1 - layer.a) * base, shown, 1.5f / 1023 + 1.0e-6f);
                bounded &= layer.a >= 0 && layer.a <= 1 && glm::all(glm::greaterThanEqual(vec3(layer), vec3(0)));
            }
        }
    }
    check(blends, "HDR10 UI recomposes to the shown PQ frame across 1331 colours");
    check(bounded, "HDR10 UI alpha is valid and premultiplied colour stays nonnegative");
    check(CompositeUiLayer(base, base, 1.5f / 1023).a == 0, "unchanged HDR10 pixels have zero UI alpha");
    bool signed_blends = true;
    for (const vec3 scene : {vec3(3,-.1f,-.05f), vec3(-.1f,2,-.02f), vec3(-.2f,-.1f,4)}) {
        for (float alpha : {0.0f,.01f,.25f,.5f,1.0f}) {
            for (const vec3 ink : {vec3(0),vec3(2.5f),vec3(3,-.1f,-.05f)}) {
                const vec3 shown = scene*(1-alpha)+ink*alpha;
                const vec4 layer = CompositeUiLayer(shown,scene,.0001f,1.0f);
                signed_blends &= near(vec3(layer)+(1-layer.a)*scene,shown) && layer.a>=0 && layer.a<=1;
            }
        }
    }
    check(signed_blends, "wide scRGB UI layer reconstructs signed scene and translucent UI without a BT709 clamp");
    return failures ? 1 : 0;
}
