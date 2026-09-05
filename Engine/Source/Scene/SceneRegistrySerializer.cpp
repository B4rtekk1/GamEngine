// The common implementation is compiled again with only the Registry API
// enabled. This keeps the scene-wrapper API and registry format logic in
// separate translation units while sharing the format helpers.
#define SCENE_SERIALIZER_REGISTRY_ONLY
#include "SceneSerializer.cpp"
