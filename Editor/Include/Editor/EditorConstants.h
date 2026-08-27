#pragma once

#include <chrono>

namespace EditorConstants {
constexpr float zero = 0.0F;
constexpr float one = 1.0F;
constexpr float half = 0.5F;
constexpr float two = 2.0F;
constexpr float three = 3.0F;
constexpr float four = 4.0F;
constexpr float five = 5.0F;
constexpr float seven = 7.0F;
constexpr float eight = 8.0F;
constexpr float nine = 9.0F;
constexpr float hitTestRadius = 9.0F;
constexpr float rotationHitTestRadius = 12.0F;
constexpr float rotationRingThickness = 6.0F;
constexpr float hoveredRotationRingThickness = 9.0F;
constexpr float twelve = 12.0F;
constexpr float thirtyTwo = 32.0F;
constexpr float fifteen = 15.0F;
constexpr float thirty = 30.0F;
constexpr float forty = 40.0F;
constexpr float sixty = 60.0F;
constexpr float seventyFive = 75.0F;
constexpr float oneHundred = 100.0F;
constexpr float oneThousand = 1000.0F;
constexpr float oneMillion = 1000000.0F;
constexpr float radiansPerDegree = 0.01745329251994329577F;
constexpr float degreesPerRadian = 57.2957795130823208768F;
constexpr float halfFovTangent = 0.57735026919F;
constexpr float epsilon = 0.0001F;
constexpr float minimumDepth = 0.01F;
constexpr float minimumDimension = 1.0F;
constexpr float snapTranslation = 0.25F;
constexpr float gizmoDesiredPixels = 112.0F;
constexpr float gizmoMinimumSize = 0.15F;
constexpr float gizmoMaximumSize = 100.0F;
constexpr int axisCount = 3;
constexpr int orientationSegmentCount = 16;
constexpr int rotationSegmentCount = 64;
constexpr double physicsStep = 1.0 / 120.0;
constexpr int maximumPhysicsStepsPerFrame = 12;
constexpr int colorAlpha = 255;
constexpr int windowWidth = 1280;
constexpr int windowHeight = 720;
constexpr int statusBarHeight = 30;
constexpr auto targetFrameMicroseconds = std::chrono::microseconds{16'667};
constexpr float viewportWidthRatio = 16.0F;
constexpr float viewportHeightRatio = 9.0F;
constexpr float cameraFieldOfView = 60.0F;
constexpr float cameraNearPlane = 0.1F;
constexpr float cameraFarPlane = 1000.0F;
}
