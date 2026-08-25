#include <Engine/Scene/Scene.h>
#include <Engine/Physics/PhysicsSystem.h>

#include <cmath>

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

    if (std::abs(dynamic.position().y() - 0.55F) > 0.001F ||
        staticBody.position().y() != 0.0F) {
        return 1;
    }

    auto sphere = scene.createActor("Rolling sphere");
    sphere.setPosition({0.0F, 0.55F, 0.0F});
    sphere.addRigidbody(RigidbodyComponent{.useGravity = false, .linearVelocity = {1.0F, 0.0F, 0.0F}});
    sphere.addSphereCollider(0.5F);

    physics.update(scene, 1.0F);
    if (std::abs(sphere.position().x() - 1.0F) > 0.001F ||
        std::abs(sphere.rotation().z() + 114.59156F) > 0.001F) {
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
    if (slopeSphere.velocity().z() >= -0.01F || slopeSphere.rotation().x() >= -0.01F) {
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
        std::abs(slidingSphere.rotation().x()) > 0.001F) {
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
    if (enteringSphere.position().z() <= -1.9F ||
        enteringSphere.position().y() <= 0.65F ||
        enteringSphere.velocity().y() <= 0.0F) {
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

    Scene rollingResistanceScene;
    auto resistanceGround = rollingResistanceScene.createActor("Ground");
    resistanceGround.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    resistanceGround.addBoxCollider({20.0F, 0.05F, 20.0F});
    auto slowingSphere = rollingResistanceScene.createActor("Slowing sphere");
    slowingSphere.setPosition({0.0F, 0.55F, 0.0F});
    slowingSphere.addRigidbody(RigidbodyComponent{
        .linearVelocity = {2.0F, 0.0F, 0.0F}
    });
    slowingSphere.addSphereCollider(0.5F);

    physics.update(rollingResistanceScene, 0.1F);
    const float speedAfterFirstStep = slowingSphere.velocity().x();
    for (int step = 0; step < 100; ++step) physics.update(rollingResistanceScene, 0.1F);
    if (speedAfterFirstStep >= 2.0F || slowingSphere.velocity().length() > 0.001F ||
        slowingSphere.position().y() < 0.549F) {
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
        std::abs(movingSphere.velocity().x() +
                 0.75F * std::exp(-0.05F * 0.1F)) > 0.001F) {
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
        slidingBox.rotation().z() >= -0.001F) {
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
    if (rampBox.rotation().x() >= -0.001F || rampBox.rotation().x() <= -44.999F ||
        rampBoxBody.angularVelocity.x() >= -0.001F ||
        std::abs(rampBox.position().y() - rampBox.position().z() - 1.0F) > 0.001F) {
        return 16;
    }
    const float firstRampBoxAngle = rampBox.rotation().x();
    for (int step = 0; step < 10; ++step) physics.update(rampBoxScene, 0.02F);
    if (!std::isfinite(rampBox.rotation().x()) || !std::isfinite(rampBox.position().y()) ||
        std::abs(rampBox.rotation().x() - firstRampBoxAngle) < 0.01F) {
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
        std::abs(thinBox.rotation().x()) < 1.0F) {
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
        maximumCubeSpeed > 2.2F || maximumCubeAngularSpeed > 140.0F) {
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
        strikingBox.position().x() + 0.5F > struckBox.position().x() - 0.5F) {
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
        laterSphere.position().y() - 0.5F < earlierBox.position().y() + 0.5F) {
        return 23;
    }

    return 0;
}