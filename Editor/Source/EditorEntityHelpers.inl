const char *entityName(const Engine::ScenePreset &scene, const Engine::Entity entity) {
    if (scene.editor().has<Engine::NameComponent>(entity)) {
        return scene.editor().get<Engine::NameComponent>(entity).value.c_str();
    }
    // A mesh can also be driven by a script. Keep the controller identity
    // visible in the hierarchy and inspector instead of hiding it behind the
    // generic GameObject label.
    if (scene.editor().has<Engine::ScriptComponent>(entity)) {
        return "Controller";
    }
    if (entity == scene.plane) {
        return "Plane";
    }
    if (entity == scene.camera) {
        return "Camera";
    }
    for (const Engine::Entity editorCube: scene.editorCubes) {
        if (editorCube == entity) {
            return "Cube";
        }
    }
    for (const Engine::Entity editorPlane: scene.editorPlanes) {
        if (editorPlane == entity) {
            return "Plane";
        }
    }
    for (const Engine::Entity editorSphere: scene.editorSpheres) {
        if (editorSphere == entity) {
            return "Sphere";
        }
    }
    for (const Engine::Entity editorCapsule: scene.editorCapsules) {
        if (editorCapsule == entity) {
            return "Capsule";
        }
    }
    for (const Engine::Entity editorRamp: scene.editorRamps) {
        if (editorRamp == entity) {
            return "Ramp";
        }
    }
    for (const Engine::Entity editorLight: scene.editorLights) {
        if (editorLight == entity) {
            return "Light";
        }
    }
    for (std::size_t index = 0; index < scene.editorGameObjects.size(); ++index) {
        if (scene.editorGameObjects[index] == entity) {
            return "GameObject";
        }
    }
    return "Entity";
}
