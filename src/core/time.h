/** @file time.h Engine timing interface. */
#pragma once

#include <chrono>

/**
 * @brief Provides frame timing and simulation timing scale for the engine.
 *
 * This is a process-wide clock. Call init() once before the first update(),
 * then call update() once per rendered frame.
 */
class Time {
public:
    /** @brief Initializes the frame timer. */
    static void init();
    /** @brief Samples the clock and updates frame and elapsed-time values. */
    static void update();

    /** @brief Returns scaled time since the previous frame, in seconds. */
    static double deltaTime();
    /** @brief Returns unscaled time since the previous frame, in seconds. */
    static double unscaledDeltaTime();
    /** @brief Returns accumulated scaled time, in seconds. */
    static double elapsedTime();

    /** @brief Returns the configured fixed simulation step, in seconds. */
    static double fixedDeltaTime();

    /** @brief Sets the multiplier applied to unscaled frame time. */
    static  void setTimeScale(double scale);
    /** @brief Returns the current time-scale multiplier. */
    static double timeScale();

    /** @brief Sets the fixed simulation step used by the game loop. */
    static void setFixedDeltaTime(double deltaTime);

private:
    static double s_deltaTime;
    static double s_unscaledDeltaTime;
    static double s_elapsedTime;

    static double s_timeScale;
    static double s_fixedDeltaTime;

    static std::chrono::steady_clock::time_point s_lastFrame;
};
