#include <Engine/Assets/Content.h>
#include <Engine/Assets/AssetTypes.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main() {
    using namespace Engine;
    using namespace Engine::Assets;

    const auto root = std::filesystem::temp_directory_path() / "gamengine_content_tests";
    const auto alternate = root / "alternate";
    std::filesystem::create_directories(alternate);
    {
        std::ofstream text(root / "readme.txt");
        text << "content facade";
        std::ofstream material(root / "material.mat");
        material << "0.1 0.2 0.3 0.4 0.8 0.25 0.9 3 4 5 0.7 1 1 0.35";
        std::ofstream alternateText(alternate / "readme.txt");
        alternateText << "alternate root";
    }

    Content content(root);
    std::vector<std::string> errors;
    content.setErrorHandler([&](const std::string& message) { errors.push_back(message); });
    if (content.assetRoot() != root) return 1;

    const auto text = content.text("readme.txt");
    const auto sameText = content.text(root / "readme.txt");
    if (!text || !sameText || text != sameText || text->text != "content facade") return 2;

    const auto material = content.material("material.mat");
    if (!material || material->baseColor.r() != 0.1f || material->baseColor.a() != 0.4f ||
        material->metallic != 0.8f || material->roughness != 0.25f ||
        material->ambientOcclusion != 0.9f || material->baseColorTexture != 3 ||
        material->normalTexture != 5 || material->normalScale != 0.7f ||
        !material->alphaBlend || !material->doubleSided || material->alphaCutoff != 0.35f) return 3;

    if (content.text("missing.txt") || errors.empty()) return 4;
    content.setAssetRoot(alternate);
    if (content.assetRoot() != alternate || !content.text("readme.txt") ||
        content.text("readme.txt")->text != "alternate root") return 5;
    content.clear();
    content.unloadUnused();

    std::filesystem::remove_all(root);
    return 0;
}
