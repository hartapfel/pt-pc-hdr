#include <cstdio>

#include "game/save_data.h"

int main() {
    using namespace pt::game;
    int failures = 0;
    const auto expect = [&](const char* locale, bool subtitles, int language) {
        const GameOptions o = DefaultOptionsForLocale(locale);
        const bool ok = o.subtitles == subtitles && o.subtitle_language == language && o.brightness == 5 && o.BrightnessValue() == 0.84f && !o.invert_x && !o.invert_y;
        std::printf("%s: subtitles %d language %d: %s\n", locale, o.subtitles, o.subtitle_language, ok ? "PASS" : "FAIL");
        failures += !ok;
    };
    expect("en-US", false, 0);
    expect("en-GB", false, 0);
    expect("ja-JP", true, 4);
    expect("fr-FR", true, 1);
    expect("fr-CA", true, 1);
    expect("es-ES", true, 3);
    expect("es-MX", true, 3);
    expect("de-DE", true, 2);
    expect("it-IT", true, 5);
    expect("pt-PT", true, 6);
    expect("pt-BR", true, 6);
    expect("tr-TR", true, 7);
    expect("zh-CN", true, 8);
    expect("zh-Hans-SG", true, 8);
    expect("ar-SA", true, 9);
    expect("ru-RU", true, 10);
    expect("uk-UA", true, 11);
    expect("cs-CZ", true, 12);
    expect("cs", true, 12);
    expect("pl", true, 13);
    expect("pl-PL", true, 13);
    expect("ko-KR", true, 0);
    expect("zh-TW", true, 0);
    expect("zh-Hant-HK", true, 0);
    expect("", true, 0);
    GameOptions options;
    float previous = 0.0f;
    bool calibration = true;
    for (int step = 0; step <= 10; ++step) {
        options.brightness = step;
        calibration &= options.BrightnessValue() > previous;
        previous = options.BrightnessValue();
    }
    options.brightness = -1;
    const float low = options.BrightnessValue();
    options.brightness = 0;
    calibration &= low == options.BrightnessValue() && low < 1.0f;
    options.brightness = 11;
    const float high = options.BrightnessValue();
    options.brightness = 10;
    calibration &= high == options.BrightnessValue() && high > 1.0f;
    std::printf("SDR calibration retains its original midpoint and bounded monotone steps: %s\n", calibration ? "PASS" : "FAIL");
    failures += !calibration;
    return failures ? 1 : 0;
}
