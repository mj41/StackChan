/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * The speaker's sample ring (PSRAM on the robot, malloc here).
 */
#include <apps/app_embody_mode/sample_ring.h>
#include <iostream>
#include <string>

namespace {
int g_failures = 0;
void expect(bool ok, const std::string& what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        g_failures++;
    }
}
}  // namespace

int main()
{
    embody::SampleRing ring;
    int16_t out[10];
    expect(ring.pop(out, 10) == 0, "an unreserved ring is empty");
    ring.push(1);  // ignored without capacity
    expect(ring.empty(), "push without capacity is ignored");

    expect(ring.reserve(4), "reserve");
    for (int16_t s = 1; s <= 6; s++) {
        ring.push(s);  // 1..6 into 4 places: 3..6 stay
    }
    expect(ring.size() == 4, "full ring keeps its capacity");
    expect(ring.pop(out, 3) == 3 && out[0] == 3 && out[1] == 4 && out[2] == 5, "the oldest are dropped");
    ring.push(7);
    ring.push(8);
    expect(ring.pop(out, 10) == 3 && out[0] == 6 && out[1] == 7 && out[2] == 8, "wraps around in order");
    expect(ring.empty(), "empty after popping all");

    ring.push(9);
    expect(ring.reserve(4) && ring.size() == 1, "the same capacity keeps the samples");
    expect(ring.reserve(8) && ring.empty() && ring.capacity() == 8, "a new capacity starts empty");
    ring.push(1);
    ring.clear();
    expect(ring.empty(), "clear");

    // fadeOut: a soft cut. Keeps the oldest samples, falling to silence.
    embody::SampleRing f;
    f.reserve(100);
    for (int i = 0; i < 50; i++) {
        f.push(1000);
    }
    f.fadeOut(4);
    expect(f.size() == 4, "fadeOut keeps 4");
    int16_t faded[4];
    f.pop(faded, 4);
    expect(faded[0] > faded[1] && faded[1] > faded[2] && faded[2] > faded[3] && faded[3] > 0 && faded[0] < 1000,
           "fadeOut ramps down: " + std::to_string(faded[0]) + " " + std::to_string(faded[3]));

    // mixInto: speech plus a sound, clipped at the 16-bit range.
    int16_t speech[3] = {1000, 30000, -30000};
    const int16_t sound[3] = {500, 10000, -10000};
    embody::mixInto(speech, sound, 3);
    expect(speech[0] == 1500 && speech[1] == 32767 && speech[2] == -32768, "mixInto adds and clips");

    if (g_failures) {
        return 1;
    }
    std::cout << "sample_ring_test: ok\n";
    return 0;
}
