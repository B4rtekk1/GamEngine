#include "../Platform/SDL/SDLInput.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/Input/Input.h"
#include "Engine/Renderer/Vulkan/CameraController.h"
#include "Engine/Scene/TransformSystem.h"

#include <gtest/gtest.h>

namespace {
class CameraControllerInput : public testing::Test {
  protected:
    void SetUp() override {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_VIDEO));
        window_ = SDL_CreateWindow("Camera input test", 320, 180, SDL_WINDOW_HIDDEN);
        ASSERT_NE(window_, nullptr);
        Engine::SDLInput::setWindow(window_);
        ASSERT_TRUE(Engine::Input::setCursorMode(Engine::CursorMode::Locked));
        Engine::Input::beginFrame();
        camera_ = registry_.create();
        registry_.add<Engine::CameraComponent>(camera_);
        registry_.add<Engine::Transform>(
            camera_, Engine::Transform{.position = {0.0F, 0.0F, 0.0F}, .rotation = {0.0F, -90.0F, 0.0F}});
        Engine::TransformSystem::updateDirty(registry_);
        controller_.setGameInputEnabled(true);
    }

    void TearDown() override {
        SDL_Event keyUp{};
        keyUp.type = SDL_EVENT_KEY_UP;
        keyUp.key.scancode = SDL_SCANCODE_W;
        keyUp.key.down = false;
        Engine::SDLInput::processEvent(keyUp);
        Engine::SDLInput::setWindow(nullptr);
        if (window_)
            SDL_DestroyWindow(window_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
        Engine::Input::beginFrame();
    }

    Engine::Registry registry_;
    Engine::CameraController controller_;
    Engine::Entity camera_{Engine::NullEntity};
    SDL_Window* window_{};
};

TEST_F(CameraControllerInput, MouseLookAndMovementInvalidateTheRenderedCameraTransform) {
    const auto beforeRevision = registry_.componentRevision<Engine::Transform>();
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 40.0F;
    motion.motion.yrel = -20.0F;
    Engine::SDLInput::processEvent(motion);
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 1.0F;
    Engine::SDLInput::processEvent(wheel);

    controller_.update(registry_);
    EXPECT_GT(registry_.componentRevision<Engine::Transform>(), beforeRevision);
    Engine::TransformSystem::updateDirty(registry_);
    const auto& pose = registry_.get<Engine::Transform>(camera_);
    EXPECT_NEAR(pose.rotation.y(), -80.0F, 0.001F);
    EXPECT_NEAR(pose.rotation.x(), 5.0F, 0.001F);
    EXPECT_NEAR(pose.worldPosition().z(), -2.0F, 0.001F);
    ASSERT_TRUE(controller_.camera());
    EXPECT_NEAR(controller_.camera()->position().z(), pose.worldPosition().z(), 0.001F);
    EXPECT_GT(controller_.camera()->forward().x(), 0.1F);

    Engine::Input::beginFrame();
    const auto idleRevision = registry_.componentRevision<Engine::Transform>();
    controller_.update(registry_);
    EXPECT_EQ(registry_.componentRevision<Engine::Transform>(), idleRevision);
}

TEST_F(CameraControllerInput, ReleasedMouseCaptureLeavesCameraUnchanged) {
    ASSERT_TRUE(Engine::Input::setCursorMode(Engine::CursorMode::Normal));
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 40.0F;
    Engine::SDLInput::processEvent(motion);
    const auto revision = registry_.componentRevision<Engine::Transform>();
    controller_.update(registry_);
    EXPECT_EQ(registry_.componentRevision<Engine::Transform>(), revision);
    EXPECT_NEAR(registry_.get<Engine::Transform>(camera_).rotation.y(), -90.0F, 0.001F);
}

TEST_F(CameraControllerInput, ForwardKeyUpdatesWorldPositionAndNotifiesTransformObservers) {
    const auto revision = registry_.componentRevision<Engine::Transform>();
    SDL_Event keyDown{};
    keyDown.type = SDL_EVENT_KEY_DOWN;
    keyDown.key.scancode = SDL_SCANCODE_W;
    keyDown.key.down = true;
    Engine::SDLInput::processEvent(keyDown);
    Engine::Time::init();
    SDL_Delay(2);
    Engine::Time::update();
    controller_.update(registry_);
    EXPECT_GT(registry_.componentRevision<Engine::Transform>(), revision);
    Engine::TransformSystem::updateDirty(registry_);
    EXPECT_LT(registry_.get<Engine::Transform>(camera_).worldPosition().z(), 0.0F);
}

TEST_F(CameraControllerInput, ParentedCameraMovementFollowsWorldViewWithParentScale) {
    const auto parent = registry_.create();
    registry_.add<Engine::UUIDComponent>(parent, Engine::UUIDComponent{1234});
    Engine::Transform pose;
    pose.position = {10.0F, 3.0F, 4.0F};
    pose.rotation = {0.0F, 90.0F, 0.0F};
    pose.scale = {2.0F, 2.0F, 2.0F};
    registry_.add<Engine::Transform>(parent, pose);
    registry_.add<Engine::ParentComponent>(camera_, Engine::ParentComponent{1234});
    Engine::TransformSystem::updateDirty(registry_);
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 1.0F;
    Engine::SDLInput::processEvent(wheel);
    controller_.update(registry_);
    const auto& cameraPose = registry_.get<Engine::Transform>(camera_);
    EXPECT_NEAR(cameraPose.worldPosition().x(), 12.0F, 0.001F);
    EXPECT_NEAR(cameraPose.worldPosition().y(), 3.0F, 0.001F);
    EXPECT_NEAR(cameraPose.worldPosition().z(), 4.0F, 0.001F);
    EXPECT_NEAR(controller_.camera()->position().x(), 12.0F, 0.001F);
}
} // namespace
