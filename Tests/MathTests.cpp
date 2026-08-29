#include <gtest/gtest.h>

#include "Engine/Core/Transform.h"
#include "Engine/Math/Math.h"

namespace {

void ExpectVec2Near(const Engine::Vec2& value, float x, float y) {
    EXPECT_NEAR(value.x(), x, 1.0e-5F);
    EXPECT_NEAR(value.y(), y, 1.0e-5F);
}

void ExpectVec3Near(const Engine::Vec3& value, float x, float y, float z) {
    EXPECT_NEAR(value.x(), x, 1.0e-5F);
    EXPECT_NEAR(value.y(), y, 1.0e-5F);
    EXPECT_NEAR(value.z(), z, 1.0e-5F);
}

void ExpectVec4Near(const Engine::Vec4& value, float x, float y, float z, float w) {
    EXPECT_NEAR(value.x(), x, 1.0e-5F);
    EXPECT_NEAR(value.y(), y, 1.0e-5F);
    EXPECT_NEAR(value.z(), z, 1.0e-5F);
    EXPECT_NEAR(value.w(), w, 1.0e-5F);
}

TEST(Vec2, SupportsArithmeticMutationAndNormalization) {
    Engine::Vec2 value{3.0F, 4.0F};
    ExpectVec2Near(value + Engine::Vec2{1.0F, -2.0F}, 4.0F, 2.0F);
    ExpectVec2Near(value - Engine::Vec2{1.0F, 2.0F}, 2.0F, 2.0F);
    ExpectVec2Near(-value, -3.0F, -4.0F);
    ExpectVec2Near(value * Engine::Vec2{2.0F, -1.0F}, 6.0F, -4.0F);
    ExpectVec2Near(2.0F * value, 6.0F, 8.0F);
    EXPECT_FLOAT_EQ(value.length(), 5.0F);
    ExpectVec2Near(value.normalized(), 0.6F, 0.8F);
    value += Engine::Vec2{1.0F, 1.0F};
    value *= 2.0F;
    ExpectVec2Near(value, 8.0F, 10.0F);
}

TEST(Vec3, SupportsArithmeticCrossDotAndNormalization) {
    Engine::Vec3 value{3.0F, 4.0F, 12.0F};
    ExpectVec3Near(value + Engine::Vec3{1.0F, -2.0F, 3.0F}, 4.0F, 2.0F, 15.0F);
    ExpectVec3Near(value * Engine::Vec3{2.0F, -1.0F, 0.5F}, 6.0F, -4.0F, 6.0F);
    ExpectVec3Near(Engine::cross({1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}), 0.0F, 0.0F, 1.0F);
    EXPECT_FLOAT_EQ(Engine::dot(Engine::Vec3{1.0F, 2.0F, 3.0F}, Engine::Vec3{4.0F, -5.0F, 6.0F}), 12.0F);
    EXPECT_FLOAT_EQ(value.length(), 13.0F);
    ExpectVec3Near(value.normalized(), 3.0F / 13.0F, 4.0F / 13.0F, 12.0F / 13.0F);
}

TEST(Vec4, SupportsArithmeticDivisionAndNormalization) {
    Engine::Vec4 value{1.0F, -2.0F, 3.0F, -4.0F};
    ExpectVec4Near(value + Engine::Vec4{1.0F, 2.0F, -3.0F, 4.0F}, 2.0F, 0.0F, 0.0F, 0.0F);
    ExpectVec4Near(value / 2.0F, 0.5F, -1.0F, 1.5F, -2.0F);
    value *= Engine::Vec4{2.0F, 3.0F, 4.0F, 5.0F};
    ExpectVec4Near(value, 2.0F, -6.0F, 12.0F, -20.0F);
    EXPECT_NEAR(value.normalized().length(), 1.0F, 1.0e-5F);
}

TEST(Angles, ConvertAndApplyArithmeticInBothUnits) {
    const Engine::Degrees degrees{180.0F};
    const Engine::Radians radians{degrees};
    EXPECT_NEAR(radians.value(), Engine::Pi, 1.0e-5F);
    EXPECT_NEAR(radians.toDegrees().value(), 180.0F, 1.0e-4F);
    EXPECT_NEAR(Engine::Degrees{Engine::Radians{Engine::HalfPi}}.value(), 90.0F, 1.0e-4F);
    EXPECT_FLOAT_EQ((Engine::Degrees{10.0F} + Engine::Degrees{5.0F}).value(), 15.0F);
    EXPECT_FLOAT_EQ((2.0F * Engine::Radians{1.5F}).value(), 3.0F);
}

TEST(Trigonometry, HandlesTypedAnglesAndInverseFunctions) {
    EXPECT_NEAR(Engine::sin(Engine::Degrees{90.0F}), 1.0F, 1.0e-5F);
    EXPECT_NEAR(Engine::cos(Engine::Radians{Engine::Pi}), -1.0F, 1.0e-5F);
    EXPECT_NEAR(Engine::tan(Engine::Degrees{45.0F}), 1.0F, 1.0e-5F);
    EXPECT_NEAR(Engine::asin(1.0F).value(), Engine::HalfPi, 1.0e-5F);
    EXPECT_NEAR(Engine::atan2(1.0F, 0.0F).value(), Engine::HalfPi, 1.0e-5F);
    EXPECT_NEAR(Engine::cot(Engine::Degrees{45.0F}), 1.0F, 1.0e-5F);
}

TEST(Color, ParsesAndRejectsInvalidHexValues) {
    const auto rgb = Engine::Color::from_hex("#8040FF");
    EXPECT_NEAR(rgb.r(), 128.0F / 255.0F, 1.0e-6F);
    EXPECT_NEAR(rgb.g(), 64.0F / 255.0F, 1.0e-6F);
    EXPECT_FLOAT_EQ(rgb.b(), 1.0F);
    EXPECT_FLOAT_EQ(rgb.a(), 1.0F);
    const auto rgba = Engine::Color::from_hex("10203040");
    EXPECT_NEAR(rgba.a(), 64.0F / 255.0F, 1.0e-6F);
    EXPECT_THROW(static_cast<void>(Engine::Color::from_hex("abcd")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(Engine::Color::from_hex("00GG00")), std::invalid_argument);
}

TEST(Color, ConvertsPackedFormatsAndClampsWhenPacking) {
    const auto rgba8 = Engine::Color::from_rgba8(0xFF800040u);
    EXPECT_FLOAT_EQ(rgba8.r(), 1.0F);
    EXPECT_NEAR(rgba8.g(), 128.0F / 255.0F, 1.0e-6F);
    EXPECT_FLOAT_EQ(rgba8.b(), 0.0F);
    EXPECT_NEAR(rgba8.a(), 64.0F / 255.0F, 1.0e-6F);
    const auto packed = Engine::Color{-1.0F, 0.5F, 2.0F, 0.5F}.to_a2b10g10r10();
    const auto unpacked = Engine::Color::from_a2b10g10r10(packed);
    EXPECT_FLOAT_EQ(unpacked.r(), 0.0F);
    EXPECT_NEAR(unpacked.g(), 0.5F, 1.0F / 1023.0F);
    EXPECT_FLOAT_EQ(unpacked.b(), 1.0F);
    EXPECT_NEAR(unpacked.a(), 2.0F / 3.0F, 1.0e-6F);
}

TEST(Color, SupportsOperationsInterpolationAndVectorConversion) {
    const Engine::Color first{0.2F, 0.4F, 0.6F, 0.8F};
    const Engine::Color second{0.6F, 0.2F, 0.0F, 0.4F};
    const auto mixed = Engine::Color::lerp(first, second, 0.25F);
    EXPECT_NEAR(mixed.r(), 0.3F, 1.0e-6F);
    EXPECT_NEAR(mixed.g(), 0.35F, 1.0e-6F);
    EXPECT_NEAR(Engine::Color::lerp(first, second, -1.0F).r(), first.r(), 1.0e-6F);
    EXPECT_NEAR(Engine::Color::lerp(first, second, 2.0F).r(), second.r(), 1.0e-6F);
    const auto multiplied = first * second;
    EXPECT_NEAR(multiplied.b(), 0.0F, 1.0e-6F);
    ExpectVec4Near(Engine::Color::from_vec4(first.to_vec4()).to_vec4(), 0.2F, 0.4F, 0.6F, 0.8F);
    auto scaled = first;
    scaled *= 2.0F;
    EXPECT_NEAR(scaled.r(), 0.4F, 1.0e-6F);
    EXPECT_NEAR(scaled.a(), 0.8F, 1.0e-6F);
}

TEST(Mat4AndQuat, BuildAndApplyTransforms) {
    const auto translated = Engine::Mat4::translate({1.0F, 2.0F, 3.0F});
    ExpectVec4Near(translated * Engine::Vec4{4.0F, 5.0F, 6.0F, 1.0F}, 5.0F, 7.0F, 9.0F, 1.0F);
    const auto rotation = Engine::Quat::angleAxis(Engine::HalfPi, {0.0F, 0.0F, 1.0F});
    ExpectVec3Near(rotation * Engine::Vec3{1.0F, 0.0F, 0.0F}, 0.0F, 1.0F, 0.0F);
    const auto transform = Engine::Mat4::scale(Engine::Mat4::translate({1.0F, 0.0F, 0.0F}), {2.0F, 3.0F, 4.0F});
    ExpectVec4Near(transform * Engine::Vec4{1.0F, 1.0F, 1.0F, 1.0F}, 3.0F, 3.0F, 4.0F, 1.0F);
}

TEST(Mat4, UsesVulkanZeroToOneDepthRange) {
    const auto projection = Engine::Mat4::ortho(-1.0F, 1.0F, -1.0F, 1.0F,
                                                 1.0F, 11.0F);
    const auto nearPoint = projection * Engine::Vec4{0.0F, 0.0F, -1.0F, 1.0F};
    const auto farPoint = projection * Engine::Vec4{0.0F, 0.0F, -11.0F, 1.0F};
    EXPECT_NEAR(nearPoint.z() / nearPoint.w(), 0.0F, 1.0e-5F);
    EXPECT_NEAR(farPoint.z() / farPoint.w(), 1.0F, 1.0e-5F);
}

TEST(TransformAndBounds, ProduceExpectedMatricesAndBounds) {
    Engine::Transform transform{};
    transform.position = {10.0F, -2.0F, 5.0F};
    transform.scale = {2.0F, 3.0F, 4.0F};
    ExpectVec4Near(transform.matrix() * Engine::Vec4{1.0F, 1.0F, 1.0F, 1.0F}, 12.0F, 1.0F, 9.0F, 1.0F);
    const auto bounds = Engine::AABB::unitCube().transformed(Engine::Mat4::translate({1.0F, 2.0F, 3.0F}).native());
    ExpectVec3Near(bounds.min, 0.5F, 1.5F, 2.5F);
    ExpectVec3Near(bounds.max, 1.5F, 2.5F, 3.5F);
}

TEST(Frustum, DistinguishesIntersectingAndOutsideBounds) {
    const Engine::Frustum frustum{Engine::Mat4{glm::mat4{1.0F}}};
    EXPECT_TRUE(frustum.intersects({{-0.5F, -0.5F, 0.1F}, {0.5F, 0.5F, 0.9F}}));
    EXPECT_TRUE(frustum.intersects({{-2.0F, -0.5F, 0.1F}, {0.0F, 0.5F, 0.9F}}));
    EXPECT_FALSE(frustum.intersects({{1.1F, -0.5F, 0.1F}, {2.0F, 0.5F, 0.9F}}));
    EXPECT_FALSE(frustum.intersects({{-0.5F, -0.5F, -2.0F}, {0.5F, 0.5F, -0.1F}}));
}

} // namespace
