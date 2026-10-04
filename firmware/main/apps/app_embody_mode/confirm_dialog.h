/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <functional>
#include <string>

namespace embody {

/**
 * @brief A Yes/No question on the robot's screen, above any app (LVGL's top layer): for what
 *        only the person at the robot may allow (a new default server, turning the head).
 *        Call with the LVGL lock held; done(true) after Yes, done(false) after No or after
 *        `seconds` without an answer, called from the LVGL task.
 */
void askOnScreen(const std::string& question, const std::string& detail, int seconds, std::function<void(bool)> done);

/**
 * @brief The same, waiting for the answer: from a task of its own (not the LVGL task, and
 *        without the LVGL lock held).
 */
bool askOnScreenAndWait(const std::string& question, const std::string& detail, int seconds);

/**
 * @brief Whether one of these questions is on the screen now. Taps injected over USB (automation)
 *        are refused then: only the person at the robot may answer.
 */
bool questionOpen();

}  // namespace embody
