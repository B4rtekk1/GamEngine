#include <Engine/Scene/Scene.h>
#include <Engine/Physics/PhysicsSystem.h>
#include <Engine/Math/Quat.h>

#include <cmath>
#include <array>

namespace {
float signedAngle(float degrees) {
    while (degrees > 180.0F) degrees -= 360.0F;
    while (degrees <= -180.0F) degrees += 360.0F;
    return degrees;
}
}

int main() {
    using namespace Engine;

    Scene scene;
    auto dynamic = scene.createActor("Dynamic");
    dynamic.setPosition({0.0F, 10.0F, 0.0F});
    dynamic.addRigidbody();
    dynamic.addBoxCollider({0.5F, 0.5F, 0.5F});

    auto staticBody = scene.createActor("Ground");
    staticBody.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    staticBody.addBoxCollider({10.0F, 0.05F, 10.0F});

    PhysicsSystem physics;
    physics.update(scene, 1.0F);

    const float dynamicRestingY = dynamic.position().y();
    if (std::abs(dynamicRestingY - 0.55F) > 0.001F ||
        staticBody.position().y() != 0.0F) {
        return 1;
    }

    auto sphere = scene.createActor("Rolling sphere");
    sphere.setPosition({2.0F, 0.55F, 0.0F});
    sphere.addRigidbody(RigidbodyComponent{.useGravity = false, .linearVelocity = {1.0F, 0.0F, 0.0F}});
    sphere.addSphereCollider(0.5F);

    physics.update(scene, 1.0F);
    if (std::abs(sphere.position().x() - 3.0F) > 0.001F ||
        std::abs(signedAngle(sphere.rotation().z())) > 0.001F) {
        return 2;
    }

    Scene slopeScene;
    auto ramp = slopeScene.createActor("Ramp");
    ramp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    ramp.addRampCollider({3.0F, 2.0F, 2.0F});
    auto slopeSphere = slopeScene.createActor("Slope sphere");
    slopeSphere.setPosition({0.0F, 0.7071068F, 0.0F});
    slopeSphere.addRigidbody();
    slopeSphere.addSphereCollider(0.5F);

    physics.update(slopeScene, 0.1F);
    if (slopeSphere.velocity().z() >= -0.01F || signedAngle(slopeSphere.rotation().x()) >= -0.01F) {
        return 3;
    }

    Scene slipperySlopeScene;
    auto slipperyRamp = slipperySlopeScene.createActor("Slippery ramp");
    slipperyRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    slipperyRamp.addRampCollider({3.0F, 2.0F, 2.0F});
    slipperyRamp.setColliderMaterial(0.0F, 0.0F);
    auto slidingSphere = slipperySlopeScene.createActor("Sliding sphere");
    slidingSphere.setPosition({0.0F, 0.7071068F, 0.0F});
    slidingSphere.addRigidbody();
    slidingSphere.addSphereCollider(0.5F);
    slidingSphere.setColliderMaterial(0.0F, 0.0F);

    physics.update(slipperySlopeScene, 0.1F);
    if (slidingSphere.velocity().z() >= -0.45F ||
        std::abs(signedAngle(slidingSphere.rotation().x())) > 0.001F) {
        return 4;
    }

    Scene connectedRampsScene;
    auto firstRamp = connectedRampsScene.createActor("First ramp");
    firstRamp.setPosition({0.0F, 2.0F, 0.0F});
    firstRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    firstRamp.addRampCollider({3.0F, 2.0F, 2.0F});
    auto secondRamp = connectedRampsScene.createActor("Second ramp");
    secondRamp.setPosition({0.0F, -2.0F, -4.0F});
    secondRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    secondRamp.addRampCollider({3.0F, 2.0F, 2.0F});
    auto connectedSphere = connectedRampsScene.createActor("Connected-ramp sphere");
    connectedSphere.setPosition({0.0F, 2.0F + 1.5F + 0.7071068F, 1.5F});
    connectedSphere.addRigidbody();
    connectedSphere.addSphereCollider(0.5F);

    for (int step = 0; step < 16; ++step) physics.update(connectedRampsScene, 0.1F);
    const float expectedSecondRampY = -2.0F + (connectedSphere.position().z() + 4.0F) + 0.7071068F;
    if (connectedSphere.position().z() >= -2.5F || connectedSphere.position().z() <= -4.0F ||
        std::abs(connectedSphere.position().y() - expectedSecondRampY) > 0.01F) {
        return 5;
    }

    Scene rampEntryScene;
    auto entryGround = rampEntryScene.createActor("Ground");
    entryGround.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    entryGround.addBoxCollider({10.0F, 0.05F, 10.0F});
    auto entryRamp = rampEntryScene.createActor("Ramp");
    entryRamp.setPosition({0.0F, 2.05F, 0.0F});
    entryRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    entryRamp.addRampCollider({3.0F, 2.0F, 2.0F});
    auto enteringSphere = rampEntryScene.createActor("Entering sphere");
    enteringSphere.setPosition({0.0F, 0.55F, -2.5F});
    enteringSphere.addRigidbody(RigidbodyComponent{
        .linearVelocity = {0.0F, 0.0F, 4.0F}
    });
    enteringSphere.addSphereCollider(0.5F);

    for (int step = 0; step < 40; ++step) physics.update(rampEntryScene, 0.01F);
    const Vec3 rampEntryPosition = enteringSphere.position();
    const Vec3 rampEntryVelocity = enteringSphere.velocity();
    if (rampEntryPosition.z() <= -1.95F ||
        rampEntryPosition.y() <= 0.65F ||
        rampEntryVelocity.y() <= 0.0F) {
        return 6;
    }

    Scene rampExitScene;
    auto exitGround = rampExitScene.createActor("Ground");
    exitGround.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    exitGround.addBoxCollider({10.0F, 0.05F, 10.0F});
    auto exitRamp = rampExitScene.createActor("Ramp");
    exitRamp.setPosition({0.0F, 2.05F, 0.0F});
    exitRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    exitRamp.addRampCollider({3.0F, 2.0F, 2.0F});
    auto exitingSphere = rampExitScene.createActor("Exiting sphere");
    exitingSphere.setPosition({0.0F, 1.05F + 0.7071068F, -1.0F});
    exitingSphere.addRigidbody();
    exitingSphere.addSphereCollider(0.5F);

    for (int step = 0; step < 120; ++step) physics.update(rampExitScene, 0.01F);
    if (exitingSphere.position().z() >= -2.5F ||
        exitingSphere.position().y() < 0.549F) {
        return 7;
    }

    // Damping is a rigidbody property and must behave identically regardless
    // of the collider shape attached to the body.
    Scene dampingScene;
    auto dampedSphere = dampingScene.createActor("Damped sphere");
    dampedSphere.setPosition({0.0F, 0.0F, -2.0F});
    dampedSphere.addRigidbody(RigidbodyComponent{
        .linearDamping = 1.0F, .angularDamping = 0.0F, .useGravity = false,
        .linearVelocity = {2.0F, 0.0F, 0.0F}
    });
    dampedSphere.addSphereCollider(0.5F);
    auto dampedBox = dampingScene.createActor("Damped box");
    dampedBox.setPosition({0.0F, 0.0F, 2.0F});
    dampedBox.addRigidbody(RigidbodyComponent{
        .linearDamping = 1.0F, .angularDamping = 0.0F, .useGravity = false,
        .linearVelocity = {2.0F, 0.0F, 0.0F}
    });
    dampedBox.addBoxCollider();

    physics.update(dampingScene, 0.5F);
    const float expectedDampedSpeed = 2.0F * std::exp(-0.5F);
    if (std::abs(dampedSphere.velocity().x() - expectedDampedSpeed) > 0.001F ||
        std::abs(dampedBox.velocity().x() - expectedDampedSpeed) > 0.001F) {
        return 8;
    }

    Scene allCollidersScene;
    auto movingSphere = allCollidersScene.createActor("Collider-testing sphere");
    movingSphere.setPosition({-1.4F, 0.0F, 0.0F});
    movingSphere.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0F, 0.0F, 0.0F}
    });
    movingSphere.addSphereCollider(0.5F);
    auto boxWithoutBody = allCollidersScene.createActor("Box without rigidbody");
    boxWithoutBody.addBoxCollider({0.5F, 0.5F, 0.5F});

    physics.update(allCollidersScene, 0.1F);
    if (movingSphere.position().x() > -0.999F ||
        std::abs(movingSphere.velocity().x()) > 0.001F) {
        return 9;
    }

    Scene sphereColliderScene;
    auto firstSphere = sphereColliderScene.createActor("Moving sphere");
    firstSphere.setPosition({-1.4F, 0.0F, 0.0F});
    firstSphere.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0F, 0.0F, 0.0F}
    });
    firstSphere.addSphereCollider(0.5F);
    auto staticSphere = sphereColliderScene.createActor("Static sphere collider");
    staticSphere.addSphereCollider(0.5F);

    physics.update(sphereColliderScene, 0.1F);
    if (firstSphere.position().x() > -0.999F || firstSphere.velocity().x() > 0.001F) {
        return 10;
    }

    Scene capsuleColliderScene;
    auto capsuleTestSphere = capsuleColliderScene.createActor("Moving sphere");
    capsuleTestSphere.setPosition({-1.4F, 0.0F, 0.0F});
    capsuleTestSphere.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0F, 0.0F, 0.0F}
    });
    capsuleTestSphere.addSphereCollider(0.5F);
    auto capsule = capsuleColliderScene.createActor("Capsule collider");
    capsule.addCapsuleCollider(0.5F, 2.0F);

    physics.update(capsuleColliderScene, 0.1F);
    if (capsuleTestSphere.position().x() > -0.999F ||
        capsuleTestSphere.velocity().x() > 0.001F) {
        return 11;
    }

    Scene kinematicColliderScene;
    auto kinematicTestSphere = kinematicColliderScene.createActor("Moving sphere");
    kinematicTestSphere.setPosition({-1.4F, 0.0F, 0.0F});
    kinematicTestSphere.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0F, 0.0F, 0.0F}
    });
    kinematicTestSphere.addSphereCollider(0.5F);
    auto kinematicBox = kinematicColliderScene.createActor("Kinematic box");
    kinematicBox.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Kinematic});
    kinematicBox.addBoxCollider({0.5F, 0.5F, 0.5F});

    physics.update(kinematicColliderScene, 0.1F);
    if (kinematicTestSphere.position().x() > -0.999F ||
        kinematicTestSphere.velocity().x() > 0.001F) {
        return 12;
    }

    Scene dynamicColliderScene;
    auto dynamicTestSphere = dynamicColliderScene.createActor("Moving sphere");
    dynamicTestSphere.setPosition({-1.4F, 0.35F, 0.0F});
    dynamicTestSphere.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0F, 0.0F, 0.0F}
    });
    dynamicTestSphere.addSphereCollider(0.5F);
    auto dynamicBox = dynamicColliderScene.createActor("Dynamic box");
    dynamicBox.addRigidbody(RigidbodyComponent{.useGravity = false});
    dynamicBox.addBoxCollider({0.5F, 0.5F, 0.5F});

    physics.update(dynamicColliderScene, 0.1F);
    if (dynamicTestSphere.velocity().x() >= 5.0F ||
        dynamicBox.velocity().x() <= 0.001F ||
        dynamicColliderScene.find(dynamicBox.id())->rigidbody().angularVelocity.z() >= -0.001F) {
        return 13;
    }

    Scene boxCollisionScene;
    auto movingBox = boxCollisionScene.createActor("Moving box");
    movingBox.setPosition({-1.4F, 0.0F, 0.0F});
    movingBox.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {5.0F, 0.0F, 0.0F}
    });
    movingBox.addBoxCollider({0.5F, 0.5F, 0.5F});
    auto boxWall = boxCollisionScene.createActor("Box wall");
    boxWall.addBoxCollider({0.5F, 2.0F, 2.0F});

    physics.update(boxCollisionScene, 0.1F);
    if (movingBox.position().x() > -0.999F || movingBox.velocity().x() > 0.001F) {
        return 14;
    }

    Scene slidingBoxScene;
    auto boxGround = slidingBoxScene.createActor("Ground");
    boxGround.addBoxCollider({10.0F, 0.05F, 10.0F});
    auto slidingBox = slidingBoxScene.createActor("Sliding box");
    slidingBox.setPosition({0.0F, 0.55F, 0.0F});
    slidingBox.addRigidbody(RigidbodyComponent{
        .linearVelocity = {2.0F, 0.0F, 0.0F}
    });
    slidingBox.addBoxCollider();

    physics.update(slidingBoxScene, 0.1F);
    if (slidingBox.velocity().x() >= 2.0F || slidingBox.position().y() < 0.549F ||
        signedAngle(slidingBox.rotation().z()) >= -0.001F) {
        return 15;
    }

    Scene rampBoxScene;
    auto boxRamp = rampBoxScene.createActor("Ramp");
    boxRamp.addRampCollider({3.0F, 2.0F, 2.0F});
    auto rampBox = rampBoxScene.createActor("Ramp box");
    rampBox.setPosition({0.0F, 1.0F, 0.0F});
    rampBox.addRigidbody();
    rampBox.addBoxCollider();

    physics.update(rampBoxScene, 0.1F);
    const auto& rampBoxBody = rampBoxScene.find(rampBox.id())->rigidbody();
    if (signedAngle(rampBox.rotation().x()) >= -0.001F ||
        signedAngle(rampBox.rotation().x()) <= -44.999F ||
        rampBoxBody.angularVelocity.x() >= -0.001F ||
        std::abs(rampBox.position().y() - rampBox.position().z() - 1.0F) > 0.001F) {
        return 16;
    }
    const float firstRampBoxAngle = rampBox.rotation().x();
    for (int step = 0; step < 10; ++step) physics.update(rampBoxScene, 0.02F);
    if (!std::isfinite(rampBox.rotation().x()) || !std::isfinite(rampBox.position().y()) ||
        std::abs(signedAngle(rampBox.rotation().x() - firstRampBoxAngle)) < 0.01F) {
        return 17;
    }

    Scene topplingScene;
    auto topplingGround = topplingScene.createActor("Ground");
    topplingGround.addBoxCollider({100.0F, 0.05F, 100.0F});
    auto topplingSphere = topplingScene.createActor("Heavy sphere");
    topplingSphere.setPosition({0.0F, 0.55F, 1.5F});
    topplingSphere.addRigidbody(RigidbodyComponent{
        .mass = 10.0F, .linearVelocity = {0.0F, 0.0F, -10.0F}
    });
    topplingSphere.addSphereCollider(0.5F);
    auto thinBox = topplingScene.createActor("Tall thin box");
    thinBox.setPosition({0.0F, 1.55F, 0.0F});
    thinBox.addRigidbody();
    thinBox.addBoxCollider({1.5F, 1.5F, 0.15F});

    for (int step = 0; step < 400; ++step) {
        physics.update(topplingScene, 0.005F);
    }
    if (topplingSphere.position().y() < 0.499F ||
        !std::isfinite(thinBox.position().y()) || thinBox.position().y() > 3.0F ||
        std::abs(signedAngle(thinBox.rotation().x())) < 1.0F) {
        return 18;
    }

    Scene rampEdgeScene;
    auto solidRamp = rampEdgeScene.createActor("Ramp");
    solidRamp.addRampCollider({3.0F, 2.0F, 2.0F});
    auto edgeBox = rampEdgeScene.createActor("Box entering ramp end");
    edgeBox.setPosition({0.0F, 0.0F, 2.6F});
    edgeBox.addRigidbody(RigidbodyComponent{
        .useGravity = false, .linearVelocity = {0.0F, 0.0F, -3.0F}
    });
    edgeBox.addBoxCollider();

    physics.update(rampEdgeScene, 0.1F);
    if (edgeBox.position().z() < 2.499F || edgeBox.velocity().z() < -0.001F) {
        return 19;
    }

    Scene editorPhysicsScene;
    auto editorGround = editorPhysicsScene.createActor("Ground");
    editorGround.addBoxCollider({20.625F, 0.05F, 20.625F});
    auto firstEditorRamp = editorPhysicsScene.createActor("Ramp");
    firstEditorRamp.setPosition({0.0F, 2.0F, 0.0F});
    firstEditorRamp.addRampCollider({3.0F, 2.0F, 2.0F});
    auto secondEditorRamp = editorPhysicsScene.createActor("Ramp 2");
    secondEditorRamp.setPosition({0.0F, 2.0F, -10.0F});
    secondEditorRamp.setRotation({0.0F, 180.0F, 0.0F});
    secondEditorRamp.addRampCollider({3.0F, 2.0F, 2.0F});
    auto editorSphere = editorPhysicsScene.createActor("Sphere");
    editorSphere.setPosition({1.2F, 6.0F, 0.0F});
    editorSphere.addRigidbody(RigidbodyComponent{.mass = 10.0F});
    editorSphere.addSphereCollider(0.5F);
    auto editorCube = editorPhysicsScene.createActor("Cube");
    editorCube.setPosition({0.0F, 0.5F, -7.0F});
    editorCube.setScale({3.0F, 3.0F, 0.3F});
    editorCube.addRigidbody(RigidbodyComponent{
        .mass = 52.0F, .linearDamping = 0.5F, .angularDamping = 2.0F
    });
    editorCube.addBoxCollider();

    float maximumCubeSpeed = 0.0F;
    float maximumCubeAngularSpeed = 0.0F;
    float minimumCubeZ = editorCube.position().z();
    int maximumCubeSpeedStep = 0;
    int maximumCubeAngularSpeedStep = 0;
    for (int step = 0; step < 1200; ++step) {
        physics.update(editorPhysicsScene, 1.0F / 120.0F);
        const float cubeSpeed = editorCube.velocity().length();
        const float cubeAngularSpeed =
            editorPhysicsScene.find(editorCube.id())->rigidbody().angularVelocity.length();
        minimumCubeZ = std::min(minimumCubeZ, editorCube.position().z());
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
        maximumCubeSpeed > 2.2F || maximumCubeAngularSpeed > 60.0F) {
        return 20;
    }
    const auto& settledCubeBody =
        editorPhysicsScene.find(editorCube.id())->rigidbody();
    if (std::abs(settledCubeBody.linearVelocity.y()) > 0.01F ||
        settledCubeBody.angularVelocity.length() > 0.01F) {
        return 21;
    }
    if (minimumCubeZ < -7.3F) return 24;

    // Dynamic-vs-dynamic response must not depend on a sphere-specific path.
    // Two boxes use the same mass-weighted correction and impulse solver as
    // every other supported collider pair.
    Scene dynamicBoxesScene;
    auto strikingBox = dynamicBoxesScene.createActor("Striking box");
    strikingBox.setPosition({-1.4F, 0.0F, 0.0F});
    strikingBox.addRigidbody(RigidbodyComponent{
        .linearDamping = 0.0F,
        .angularDamping = 0.0F,
        .useGravity = false,
        .linearVelocity = {5.0F, 0.0F, 0.0F}
    });
    strikingBox.addBoxCollider();
    auto struckBox = dynamicBoxesScene.createActor("Struck box");
    struckBox.addRigidbody(RigidbodyComponent{
        .linearDamping = 0.0F,
        .angularDamping = 0.0F,
        .useGravity = false
    });
    struckBox.addBoxCollider();

    physics.update(dynamicBoxesScene, 0.1F);
    if (strikingBox.velocity().x() >= 4.999F ||
        struckBox.velocity().x() <= 0.001F ||
        strikingBox.position().x() + 0.5F > struckBox.position().x() - 0.5F + 0.001F) {
        return 22;
    }

    // Regression for Editor.scene: Cube 2 has a lower entity id than
    // Sphere 2. Collision ownership must therefore be based on supported
    // shape pairs, not simply on entity ordering.
    Scene laterSphereScene;
    auto earlierBox = laterSphereScene.createActor("Earlier dynamic box");
    earlierBox.addRigidbody(RigidbodyComponent{
        .linearDamping = 0.0F,
        .angularDamping = 0.0F,
        .useGravity = false
    });
    earlierBox.addBoxCollider();
    auto laterSphere = laterSphereScene.createActor("Later dynamic sphere");
    laterSphere.setPosition({0.0F, 1.4F, 0.0F});
    laterSphere.addRigidbody(RigidbodyComponent{
        .linearDamping = 0.0F,
        .angularDamping = 0.0F,
        .useGravity = false,
        .linearVelocity = {0.0F, -5.0F, 0.0F}
    });
    laterSphere.addSphereCollider(0.5F);

    physics.update(laterSphereScene, 0.1F);
    if (laterSphere.velocity().y() <= -4.999F ||
        earlierBox.velocity().y() >= -0.001F ||
        laterSphere.position().y() - 0.5F < earlierBox.position().y() + 0.5F - 0.001F) {
        return 23;
    }

    // Full physical layout from Assets/Scenes/Editor.scene. In particular,
    // exercise the stacked Cube 2 + Sphere 2 pair above the ramp and retain
    // the large saved rotation that previously amplified contact instability.
    Scene completeEditorScene;
    auto completeGround = completeEditorScene.createActor("Plane");
    completeGround.setScale({41.25F, 1.0F, 41.25F});
    completeGround.addBoxCollider({0.5F, 0.05F, 0.5F});

    auto completeCamera = completeEditorScene.createActor("Camera");
    completeCamera.setPosition({14.0F, 8.0F, -5.0F});
    completeCamera.setRotation({-23.1986F, 180.0F, 0.0F});

    auto completeRamp2 = completeEditorScene.createActor("Ramp 2");
    completeRamp2.setPosition({0.0F, 2.0F, -10.0F});
    completeRamp2.setRotation({0.0F, 180.0F, 0.0F});
    completeRamp2.addRampCollider({3.0F, 2.0F, 2.0F});

    auto completeSphere = completeEditorScene.createActor("Sphere");
    completeSphere.setPosition({1.2F, 6.0F, 0.0F});
    completeSphere.setRotation({0.0F, 0.0F, -2.5F});
    completeSphere.addRigidbody(RigidbodyComponent{.mass = 10.0F});
    completeSphere.addSphereCollider(0.5F);

    auto completeRamp = completeEditorScene.createActor("Ramp");
    completeRamp.setPosition({0.0F, 2.0F, 0.0F});
    completeRamp.addRampCollider({3.0F, 2.0F, 2.0F});

    auto completeCube = completeEditorScene.createActor("Cube");
    completeCube.setPosition({0.0F, 0.5F, -7.0F});
    completeCube.setScale({3.0F, 3.0F, 0.3F});
    completeCube.addRigidbody(RigidbodyComponent{
        .mass = 52.0F, .linearDamping = 0.5F, .angularDamping = 2.0F
    });
    completeCube.addBoxCollider();

    auto completeCube2 = completeEditorScene.createActor("Cube 2");
    completeCube2.setPosition({0.0F, 5.0F, 0.0F});
    completeCube2.setRotation({481.72586F, -30.386904F, 58.904774F});
    completeCube2.addRigidbody();
    completeCube2.addBoxCollider();

    auto completeSphere2 = completeEditorScene.createActor("Sphere 2");
    completeSphere2.setPosition({0.0F, 7.0F, 0.0F});
    completeSphere2.addRigidbody();
    completeSphere2.addSphereCollider(0.5F);

    const std::array<Actor, 4> completeDynamicBodies{
        completeSphere, completeCube, completeCube2, completeSphere2
    };
    float completeMaximumSpeed = 0.0F;
    float completeMaximumAngularSpeed = 0.0F;
    float completeMaximumBoxAngularSpeed = 0.0F;
    for (int simulationStep = 0; simulationStep < 1200; ++simulationStep) {
        physics.update(completeEditorScene, 1.0F / 120.0F);
        for (const Actor& actor : completeDynamicBodies) {
            const auto& rigidbody = completeEditorScene.find(actor.id())->rigidbody();
            completeMaximumSpeed = std::max(completeMaximumSpeed,
                rigidbody.linearVelocity.length());
            completeMaximumAngularSpeed = std::max(completeMaximumAngularSpeed,
                rigidbody.angularVelocity.length());
            if (!std::isfinite(actor.position().x()) || !std::isfinite(actor.position().y()) ||
                !std::isfinite(actor.position().z()) || !std::isfinite(actor.rotation().x()) ||
                actor.position().length() > 50.0F) {
                return 25;
            }
        }
        completeMaximumBoxAngularSpeed = std::max(completeMaximumBoxAngularSpeed,
            completeEditorScene.find(completeCube.id())->rigidbody().angularVelocity.length());
        completeMaximumBoxAngularSpeed = std::max(completeMaximumBoxAngularSpeed,
            completeEditorScene.find(completeCube2.id())->rigidbody().angularVelocity.length());
    }
    const float completeCubeY = completeCube.position().y();
    const float completeCube2Y = completeCube2.position().y();
    const float completeSphereY = completeSphere.position().y();
    const float completeSphere2Y = completeSphere2.position().y();
    if (completeMaximumSpeed > 20.0F || completeMaximumAngularSpeed > 1000.0F ||
        completeMaximumBoxAngularSpeed > 400.0F ||
        completeCubeY < 0.0F || completeCube2Y < 0.0F ||
        completeSphereY < 0.0F || completeSphere2Y < 0.0F) {
        return 26;
    }

    // Torque and angular impulse use the collider's inertia tensor and radians
    // internally, while the public angular velocity remains degrees/second.
    Scene angularInputScene;
    auto compactBox = angularInputScene.createActor("Compact torque box");
    compactBox.addRigidbody(RigidbodyComponent{
        .mass = 2.0F, .linearDamping = 0.0F, .angularDamping = 0.0F,
        .useGravity = false
    });
    compactBox.addBoxCollider({0.5F, 0.5F, 0.5F});
    auto wideBox = angularInputScene.createActor("Wide torque box");
    wideBox.setPosition({10.0F, 0.0F, 0.0F});
    wideBox.addRigidbody(RigidbodyComponent{
        .mass = 2.0F, .linearDamping = 0.0F, .angularDamping = 0.0F,
        .useGravity = false
    });
    wideBox.addBoxCollider({1.0F, 1.0F, 0.5F});
    angularInputScene.find(compactBox.id())->rigidbody().addTorque({0.0F, 0.0F, 1.0F});
    angularInputScene.find(wideBox.id())->rigidbody().addTorque({0.0F, 0.0F, 1.0F});
    physics.update(angularInputScene, 0.1F);
    const float compactAngularSpeed =
        angularInputScene.find(compactBox.id())->rigidbody().angularVelocity.z();
    const float wideAngularSpeed =
        angularInputScene.find(wideBox.id())->rigidbody().angularVelocity.z();
    if (std::abs(compactAngularSpeed - 17.188734F) > 0.001F ||
        std::abs(wideAngularSpeed - 4.2971835F) > 0.001F) {
        return 27;
    }
    angularInputScene.find(compactBox.id())->rigidbody().addAngularImpulse(
        {0.0F, 0.0F, 1.0F});
    physics.update(angularInputScene, 0.1F);
    const float afterAngularImpulse =
        angularInputScene.find(compactBox.id())->rigidbody().angularVelocity.z();
    physics.update(angularInputScene, 0.1F);
    if (std::abs(afterAngularImpulse - 189.07608F) > 0.002F ||
        std::abs(angularInputScene.find(compactBox.id())->rigidbody().angularVelocity.z() -
                 afterAngularImpulse) > 0.001F) {
        return 28;
    }

    // Parallel thin OBBs can have overlapping AABBs without touching.
    Scene orientedBoxesScene;
    auto firstThinBox = orientedBoxesScene.createActor("First thin OBB");
    firstThinBox.setRotation({0.0F, 0.0F, 45.0F});
    firstThinBox.addRigidbody(RigidbodyComponent{.useGravity = false});
    firstThinBox.addBoxCollider({2.0F, 0.1F, 0.1F});
    auto secondThinBox = orientedBoxesScene.createActor("Second thin OBB");
    secondThinBox.setPosition({-0.2828427F, 0.2828427F, 0.0F});
    secondThinBox.setRotation({0.0F, 0.0F, 45.0F});
    secondThinBox.addBoxCollider({2.0F, 0.1F, 0.1F});
    physics.update(orientedBoxesScene, 0.1F);
    if (firstThinBox.position().length() > 0.001F) return 29;

    // The sphere radius is uniform and uses the largest transform scale.
    Scene scaledSphereScene;
    auto scaledSphere = scaledSphereScene.createActor("Scaled sphere");
    scaledSphere.setPosition({-2.4F, 0.0F, 0.0F});
    scaledSphere.setScale({2.0F, 1.0F, 1.0F});
    scaledSphere.addRigidbody(RigidbodyComponent{
        .linearDamping = 0.0F, .angularDamping = 0.0F, .useGravity = false,
        .linearVelocity = {10.0F, 0.0F, 0.0F}
    });
    scaledSphere.addSphereCollider(0.5F);
    auto scaledSphereWall = scaledSphereScene.createActor("Wall");
    scaledSphereWall.addBoxCollider({0.5F, 2.0F, 2.0F});
    physics.update(scaledSphereScene, 0.1F);
    if (scaledSphere.position().x() > -1.499F || scaledSphere.velocity().x() > 0.001F) {
        return 30;
    }

    // Swept sphere contact prevents crossing a thin wall in one fixed step.
    Scene continuousScene;
    auto fastSphere = continuousScene.createActor("Fast sphere");
    fastSphere.setPosition({-2.0F, 0.0F, 0.0F});
    fastSphere.addRigidbody(RigidbodyComponent{
        .linearDamping = 0.0F, .angularDamping = 0.0F, .useGravity = false,
        .linearVelocity = {50.0F, 0.0F, 0.0F}
    });
    fastSphere.addSphereCollider(0.1F);
    auto thinWall = continuousScene.createActor("Thin wall");
    thinWall.addBoxCollider({0.05F, 2.0F, 2.0F});
    physics.update(continuousScene, 0.1F);
    if (fastSphere.position().x() > -0.149F || fastSphere.velocity().x() > 0.001F) {
        return 31;
    }

    // Angular velocity is expressed in world space by the impulse solver.
    // Integrating it by adding directly to Euler components mixes axes for an
    // already-rotated body; the orientation update must compose quaternions.
    Scene orientationScene;
    auto rotatingBody = orientationScene.createActor("World-space rotation");
    const Vec3 initialEuler{35.0F, -20.0F, 70.0F};
    const Vec3 worldAngularVelocity{90.0F, 40.0F, -30.0F};
    rotatingBody.setRotation(initialEuler);
    rotatingBody.addRigidbody(RigidbodyComponent{
        .linearDamping = 0.0F,
        .angularDamping = 0.0F,
        .useGravity = false,
        .angularVelocity = worldAngularVelocity
    });
    const auto orientationFromEuler = [](const Vec3& degrees) {
        constexpr float DegreesToRadians = 0.017453292519943295F;
        return Quat::angleAxis(degrees.x() * DegreesToRadians, {1.0F, 0.0F, 0.0F}) *
            Quat::angleAxis(degrees.y() * DegreesToRadians, {0.0F, 1.0F, 0.0F}) *
            Quat::angleAxis(degrees.z() * DegreesToRadians, {0.0F, 0.0F, 1.0F});
    };
    const float angularSpeedDegrees = worldAngularVelocity.length();
    const Quat expectedOrientation = Quat::angleAxis(
        angularSpeedDegrees * 0.1F * 0.017453292519943295F,
        worldAngularVelocity * (1.0F / angularSpeedDegrees)) *
        orientationFromEuler(initialEuler);
    physics.update(orientationScene, 0.1F);
    const Quat actualOrientation = orientationFromEuler(rotatingBody.rotation());
    const Vec3 unitX{1.0F, 0.0F, 0.0F};
    const Vec3 unitY{0.0F, 1.0F, 0.0F};
    if ((expectedOrientation * unitX - actualOrientation * unitX).length() > 0.001F ||
        (expectedOrientation * unitY - actualOrientation * unitY).length() > 0.001F) {
        return 32;
    }

    return 0;
}
