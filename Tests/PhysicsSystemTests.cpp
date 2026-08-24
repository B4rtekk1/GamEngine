#include <Engine/Scene/Scene.h>
#include <Engine/Physics/PhysicsSystem.h>

#include <cmath>

int main() {
    using namespace Engine;

    Scene scene;
    auto dynamic = scene.createActor("Dynamic");
    dynamic.setPosition({0.0f, 10.0f, 0.0f});
    dynamic.addRigidbody();
    dynamic.addBoxCollider({0.5f, 0.5f, 0.5f});

    auto staticBody = scene.createActor("Ground");
    staticBody.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    staticBody.addBoxCollider({10.0f, 0.05f, 10.0f});

    PhysicsSystem physics;
    physics.update(scene, 1.0f);

    if (std::abs(dynamic.position().y() - 0.55f) > 0.001f ||
        staticBody.position().y() != 0.0f) {
        return 1;
    }

    auto sphere = scene.createActor("Rolling sphere");
    sphere.setPosition({0.0f, 0.55f, 0.0f});
    sphere.addRigidbody(RigidbodyComponent{.useGravity = false, .linearVelocity = {1.0f, 0.0f, 0.0f}});
    sphere.addSphereCollider(0.5f);

    physics.update(scene, 1.0f);
    if (std::abs(sphere.position().x() - 1.0f) > 0.001f ||
        std::abs(sphere.rotation().z() + 114.59156f) > 0.001f) {
        return 2;
    }

    Scene slopeScene;
    auto ramp = slopeScene.createActor("Ramp");
    ramp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    ramp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto slopeSphere = slopeScene.createActor("Slope sphere");
    slopeSphere.setPosition({0.0f, 0.7071068f, 0.0f});
    slopeSphere.addRigidbody();
    slopeSphere.addSphereCollider(0.5f);

    physics.update(slopeScene, 0.1f);
    if (slopeSphere.velocity().z() >= -0.01f || slopeSphere.rotation().x() >= -0.01f) {
        return 3;
    }

    Scene slipperySlopeScene;
    auto slipperyRamp = slipperySlopeScene.createActor("Slippery ramp");
    slipperyRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    slipperyRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    slipperyRamp.setColliderMaterial(0.0f, 0.0f);
    auto slidingSphere = slipperySlopeScene.createActor("Sliding sphere");
    slidingSphere.setPosition({0.0f, 0.7071068f, 0.0f});
    slidingSphere.addRigidbody();
    slidingSphere.addSphereCollider(0.5f);
    slidingSphere.setColliderMaterial(0.0f, 0.0f);

    physics.update(slipperySlopeScene, 0.1f);
    if (slidingSphere.velocity().z() >= -0.45f ||
        std::abs(slidingSphere.rotation().x()) > 0.001f) {
        return 4;
    }

    Scene connectedRampsScene;
    auto firstRamp = connectedRampsScene.createActor("First ramp");
    firstRamp.setPosition({0.0f, 2.0f, 0.0f});
    firstRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    firstRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto secondRamp = connectedRampsScene.createActor("Second ramp");
    secondRamp.setPosition({0.0f, -2.0f, -4.0f});
    secondRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    secondRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto connectedSphere = connectedRampsScene.createActor("Connected-ramp sphere");
    connectedSphere.setPosition({0.0f, 2.0f + 1.5f + 0.7071068f, 1.5f});
    connectedSphere.addRigidbody();
    connectedSphere.addSphereCollider(0.5f);

    for (int step = 0; step < 16; ++step) physics.update(connectedRampsScene, 0.1f);
    const float expectedSecondRampY = -2.0f + (connectedSphere.position().z() + 4.0f) + 0.7071068f;
    if (connectedSphere.position().z() >= -2.5f || connectedSphere.position().z() <= -4.0f ||
        std::abs(connectedSphere.position().y() - expectedSecondRampY) > 0.01f) {
        return 5;
    }

    Scene rampEntryScene;
    auto entryGround = rampEntryScene.createActor("Ground");
    entryGround.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    entryGround.addBoxCollider({10.0f, 0.05f, 10.0f});
    auto entryRamp = rampEntryScene.createActor("Ramp");
    entryRamp.setPosition({0.0f, 2.05f, 0.0f});
    entryRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    entryRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto enteringSphere = rampEntryScene.createActor("Entering sphere");
    enteringSphere.setPosition({0.0f, 0.55f, -2.5f});
    enteringSphere.addRigidbody(RigidbodyComponent{
        .linearVelocity = {0.0f, 0.0f, 4.0f}
    });
    enteringSphere.addSphereCollider(0.5f);

    for (int step = 0; step < 40; ++step) physics.update(rampEntryScene, 0.01f);
    if (enteringSphere.position().z() <= -1.9f ||
        enteringSphere.position().y() <= 0.65f ||
        enteringSphere.velocity().y() <= 0.0f) {
        return 6;
    }

    Scene rampExitScene;
    auto exitGround = rampExitScene.createActor("Ground");
    exitGround.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    exitGround.addBoxCollider({10.0f, 0.05f, 10.0f});
    auto exitRamp = rampExitScene.createActor("Ramp");
    exitRamp.setPosition({0.0f, 2.05f, 0.0f});
    exitRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    exitRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto exitingSphere = rampExitScene.createActor("Exiting sphere");
    exitingSphere.setPosition({0.0f, 1.05f + 0.7071068f, -1.0f});
    exitingSphere.addRigidbody();
    exitingSphere.addSphereCollider(0.5f);

    for (int step = 0; step < 120; ++step) physics.update(rampExitScene, 0.01f);
    if (exitingSphere.position().z() >= -2.5f ||
        exitingSphere.position().y() < 0.549f) {
        return 7;
    }

    Scene rollingResistanceScene;
    auto resistanceGround = rollingResistanceScene.createActor("Ground");
    resistanceGround.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    resistanceGround.addBoxCollider({20.0f, 0.05f, 20.0f});
    auto slowingSphere = rollingResistanceScene.createActor("Slowing sphere");
    slowingSphere.setPosition({0.0f, 0.55f, 0.0f});
    slowingSphere.addRigidbody(RigidbodyComponent{
        .linearVelocity = {2.0f, 0.0f, 0.0f}
    });
    slowingSphere.addSphereCollider(0.5f);

    physics.update(rollingResistanceScene, 0.1f);
    const float speedAfterFirstStep = slowingSphere.velocity().x();
    for (int step = 0; step < 100; ++step) physics.update(rollingResistanceScene, 0.1f);
    if (speedAfterFirstStep >= 2.0f || slowingSphere.velocity().length() > 0.001f ||
        slowingSphere.position().y() < 0.549f) {
        return 8;
    }

    Scene allCollidersScene;
    auto movingSphere = allCollidersScene.createActor("Collider-testing sphere");
    movingSphere.setPosition({-1.4f, 0.0f, 0.0f});
    movingSphere.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0f, 0.0f, 0.0f}
    });
    movingSphere.addSphereCollider(0.5f);
    auto boxWithoutBody = allCollidersScene.createActor("Box without rigidbody");
    boxWithoutBody.addBoxCollider({0.5f, 0.5f, 0.5f});

    physics.update(allCollidersScene, 0.1f);
    if (movingSphere.position().x() > -0.999f ||
        std::abs(movingSphere.velocity().x() +
                 0.75f * std::exp(-0.05f * 0.1f)) > 0.001f) {
        return 9;
    }

    Scene sphereColliderScene;
    auto firstSphere = sphereColliderScene.createActor("Moving sphere");
    firstSphere.setPosition({-1.4f, 0.0f, 0.0f});
    firstSphere.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0f, 0.0f, 0.0f}
    });
    firstSphere.addSphereCollider(0.5f);
    auto staticSphere = sphereColliderScene.createActor("Static sphere collider");
    staticSphere.addSphereCollider(0.5f);

    physics.update(sphereColliderScene, 0.1f);
    if (firstSphere.position().x() > -0.999f || firstSphere.velocity().x() > 0.001f) {
        return 10;
    }

    Scene capsuleColliderScene;
    auto capsuleTestSphere = capsuleColliderScene.createActor("Moving sphere");
    capsuleTestSphere.setPosition({-1.4f, 0.0f, 0.0f});
    capsuleTestSphere.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0f, 0.0f, 0.0f}
    });
    capsuleTestSphere.addSphereCollider(0.5f);
    auto capsule = capsuleColliderScene.createActor("Capsule collider");
    capsule.addCapsuleCollider(0.5f, 2.0f);

    physics.update(capsuleColliderScene, 0.1f);
    if (capsuleTestSphere.position().x() > -0.999f ||
        capsuleTestSphere.velocity().x() > 0.001f) {
        return 11;
    }

    Scene kinematicColliderScene;
    auto kinematicTestSphere = kinematicColliderScene.createActor("Moving sphere");
    kinematicTestSphere.setPosition({-1.4f, 0.0f, 0.0f});
    kinematicTestSphere.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0f, 0.0f, 0.0f}
    });
    kinematicTestSphere.addSphereCollider(0.5f);
    auto kinematicBox = kinematicColliderScene.createActor("Kinematic box");
    kinematicBox.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Kinematic});
    kinematicBox.addBoxCollider({0.5f, 0.5f, 0.5f});

    physics.update(kinematicColliderScene, 0.1f);
    if (kinematicTestSphere.position().x() > -0.999f ||
        kinematicTestSphere.velocity().x() > 0.001f) {
        return 12;
    }

    Scene dynamicColliderScene;
    auto dynamicTestSphere = dynamicColliderScene.createActor("Moving sphere");
    dynamicTestSphere.setPosition({-1.4f, 0.35f, 0.0f});
    dynamicTestSphere.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0f, 0.0f, 0.0f}
    });
    dynamicTestSphere.addSphereCollider(0.5f);
    auto dynamicBox = dynamicColliderScene.createActor("Dynamic box");
    dynamicBox.addRigidbody(RigidbodyComponent{.useGravity = false});
    dynamicBox.addBoxCollider({0.5f, 0.5f, 0.5f});

    physics.update(dynamicColliderScene, 0.1f);
    if (dynamicTestSphere.velocity().x() >= 5.0f ||
        dynamicBox.velocity().x() <= 0.001f ||
        dynamicColliderScene.find(dynamicBox.id())->rigidbody().angularVelocity.z() >= -0.001f) {
        return 13;
    }

    Scene boxCollisionScene;
    auto movingBox = boxCollisionScene.createActor("Moving box");
    movingBox.setPosition({-1.4f, 0.0f, 0.0f});
    movingBox.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0f, 0.0f, 0.0f}
    });
    movingBox.addBoxCollider({0.5f, 0.5f, 0.5f});
    auto boxWall = boxCollisionScene.createActor("Box wall");
    boxWall.addBoxCollider({0.5f, 2.0f, 2.0f});

    physics.update(boxCollisionScene, 0.1f);
    if (movingBox.position().x() > -0.999f || movingBox.velocity().x() > 0.001f) {
        return 14;
    }

    Scene slidingBoxScene;
    auto boxGround = slidingBoxScene.createActor("Ground");
    boxGround.addBoxCollider({10.0f, 0.05f, 10.0f});
    auto slidingBox = slidingBoxScene.createActor("Sliding box");
    slidingBox.setPosition({0.0f, 0.55f, 0.0f});
    slidingBox.addRigidbody(RigidbodyComponent{
        .linearVelocity = {2.0f, 0.0f, 0.0f}
    });
    slidingBox.addBoxCollider();

    physics.update(slidingBoxScene, 0.1f);
    if (slidingBox.velocity().x() >= 2.0f || slidingBox.position().y() < 0.549f ||
        slidingBox.rotation().z() >= -0.001f) {
        return 15;
    }

    Scene rampBoxScene;
    auto boxRamp = rampBoxScene.createActor("Ramp");
    boxRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto rampBox = rampBoxScene.createActor("Ramp box");
    rampBox.setPosition({0.0f, 1.0f, 0.0f});
    rampBox.addRigidbody();
    rampBox.addBoxCollider();

    physics.update(rampBoxScene, 0.1f);
    const auto& rampBoxBody = rampBoxScene.find(rampBox.id())->rigidbody();
    if (rampBox.rotation().x() >= -0.001f || rampBox.rotation().x() <= -44.999f ||
        rampBoxBody.angularVelocity.x() >= -0.001f ||
        std::abs(rampBox.position().y() - rampBox.position().z() - 1.0f) > 0.001f) {
        return 16;
    }
    const float firstRampBoxAngle = rampBox.rotation().x();
    for (int step = 0; step < 10; ++step) physics.update(rampBoxScene, 0.02f);
    if (!std::isfinite(rampBox.rotation().x()) || !std::isfinite(rampBox.position().y()) ||
        std::abs(rampBox.rotation().x() - firstRampBoxAngle) < 0.01f) {
        return 17;
    }

    Scene topplingScene;
    auto topplingGround = topplingScene.createActor("Ground");
    topplingGround.addBoxCollider({100.0f, 0.05f, 100.0f});
    auto topplingSphere = topplingScene.createActor("Heavy sphere");
    topplingSphere.setPosition({0.0f, 0.55f, 1.5f});
    topplingSphere.addRigidbody(RigidbodyComponent{
        .mass = 10.0f, .linearVelocity = {0.0f, 0.0f, -10.0f}
    });
    topplingSphere.addSphereCollider(0.5f);
    auto thinBox = topplingScene.createActor("Tall thin box");
    thinBox.setPosition({0.0f, 1.55f, 0.0f});
    thinBox.addRigidbody();
    thinBox.addBoxCollider({1.5f, 1.5f, 0.15f});

    for (int step = 0; step < 400; ++step) {
        physics.update(topplingScene, 0.005f);
    }
    if (topplingSphere.position().y() < 0.499f ||
        !std::isfinite(thinBox.position().y()) || thinBox.position().y() > 3.0f ||
        std::abs(thinBox.rotation().x()) < 1.0f) {
        return 18;
    }

    Scene rampEdgeScene;
    auto solidRamp = rampEdgeScene.createActor("Ramp");
    solidRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto edgeBox = rampEdgeScene.createActor("Box entering ramp end");
    edgeBox.setPosition({0.0f, 0.0f, 2.6f});
    edgeBox.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {0.0f, 0.0f, -3.0f}
    });
    edgeBox.addBoxCollider();

    physics.update(rampEdgeScene, 0.1f);
    if (edgeBox.position().z() < 2.499f || edgeBox.velocity().z() < -0.001f) {
        return 19;
    }

    Scene editorPhysicsScene;
    auto editorGround = editorPhysicsScene.createActor("Ground");
    editorGround.addBoxCollider({20.625f, 0.05f, 20.625f});
    auto firstEditorRamp = editorPhysicsScene.createActor("Ramp");
    firstEditorRamp.setPosition({0.0f, 2.0f, 0.0f});
    firstEditorRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto secondEditorRamp = editorPhysicsScene.createActor("Ramp 2");
    secondEditorRamp.setPosition({0.0f, 2.0f, -10.0f});
    secondEditorRamp.setRotation({0.0f, 180.0f, 0.0f});
    secondEditorRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto editorSphere = editorPhysicsScene.createActor("Sphere");
    editorSphere.setPosition({1.2f, 6.0f, 0.0f});
    editorSphere.addRigidbody(RigidbodyComponent{.mass = 10.0f});
    editorSphere.addSphereCollider(0.5f);
    auto editorCube = editorPhysicsScene.createActor("Cube");
    editorCube.setPosition({0.0f, 0.5f, -7.0f});
    editorCube.setScale({3.0f, 3.0f, 0.3f});
    editorCube.addRigidbody(RigidbodyComponent{
        .mass = 10.0f, .linearDamping = 0.5f, .angularDamping = 2.0f
    });
    editorCube.addBoxCollider();

    float maximumCubeSpeed = 0.0f;
    float maximumCubeAngularSpeed = 0.0f;
    int maximumCubeSpeedStep = 0;
    int maximumCubeAngularSpeedStep = 0;
    for (int step = 0; step < 1200; ++step) {
        physics.update(editorPhysicsScene, 1.0f / 120.0f);
        const float cubeSpeed = editorCube.velocity().length();
        const float cubeAngularSpeed =
            editorPhysicsScene.find(editorCube.id())->rigidbody().angularVelocity.length();
        if (cubeSpeed > maximumCubeSpeed) {
            maximumCubeSpeed = cubeSpeed;
            maximumCubeSpeedStep = step;
        }
        if (cubeAngularSpeed > maximumCubeAngularSpeed) {
            maximumCubeAngularSpeed = cubeAngularSpeed;
            maximumCubeAngularSpeedStep = step;
        }
    }
    if (!std::isfinite(maximumCubeSpeed) || !std::isfinite(maximumCubeAngularSpeed) ||
        maximumCubeSpeed > 2.2f || maximumCubeAngularSpeed > 140.0f) {
        return 20;
    }
    const auto& settledCubeBody =
        editorPhysicsScene.find(editorCube.id())->rigidbody();
    if (std::abs(settledCubeBody.linearVelocity.y()) > 0.01f ||
        settledCubeBody.angularVelocity.length() > 0.01f) {
        return 21;
    }

    return 0;
}
