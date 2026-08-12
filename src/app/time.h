#include <chrono>

class Time {
public:
    static void init();
    static void update();

    static double deltaTime();
    static double unscaledDeltaTime();
    static double elapsedTime();

    static double fixedDeltaTime();

    static  void setTimeScale(double scale);
    static double timeScale();

    static void setFixedDeltaTime(double deltaTime);

private:
    static double s_deltaTime;
    static double s_unscaledDeltaTime;
    static double s_elapsedTime;

    static double s_timeScale;
    static double s_fixedDeltaTime;

    static std::chrono::steady_clock::time_point s_lastFrame;
};
