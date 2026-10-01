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

    if (g_failures) {
        return 1;
    }
    std::cout << "sample_ring_test: ok\n";
    return 0;
}
