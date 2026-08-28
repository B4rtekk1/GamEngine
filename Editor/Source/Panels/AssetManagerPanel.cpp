#include "Editor/Panels/AssetManagerPanel.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct AssetEntry final {
    std::filesystem::path relative;
};

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool is_model(const std::filesystem::path& path) {
    const auto extension = lower(path.extension().string());
    return extension == ".gltf" || extension == ".glb";
}

std::string decode_uri(std::string value) {
    for (std::size_t i = 0; i + 2 < value.size();) {
        if (value[i] != '%') {
            ++i;
            continue;
        }
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int high = hex(value[i + 1]);
        const int low = hex(value[i + 2]);
        if (high < 0 || low < 0) {
            ++i;
            continue;
        }
        value.replace(i, 3, 1, static_cast<char>((high << 4) | low));
        ++i;
    }
    return value;
}

std::vector<std::filesystem::path> gltf_dependencies(const std::filesystem::path& model) {
    std::vector<std::filesystem::path> result;
    if (lower(model.extension().string()) != ".gltf") return result;
    std::ifstream file(model);
    if (!file) return result;
    const std::string source{std::istreambuf_iterator<char>{file}, {}};
    const std::regex uriPattern{R"REGEX("uri"\s*:\s*"([^"]+)")REGEX"};
    for (std::sregex_iterator it{source.begin(), source.end(), uriPattern}, end; it != end; ++it) {
        std::string uri = decode_uri((*it)[1].str());
        if (uri.starts_with("data:") || uri.starts_with("#")) continue;
        if (const auto query = uri.find_first_of("?#"); query != std::string::npos) uri.erase(query);
        const auto path = (model.parent_path() / std::filesystem::path{uri}).lexically_normal();
        if (std::ranges::find(result, path) == result.end()) result.push_back(path);
    }
    return result;
}

std::vector<AssetEntry> scan_assets(const std::filesystem::path& root) {
    std::vector<AssetEntry> result;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error)) {
        if (error) break;
        if (!entry.is_regular_file(error)) continue;
        // The manager is an object browser. Textures and buffers are shown
        // below the selected model as dependencies, not as spawnable assets.
        if (!is_model(entry.path())) continue;
        result.push_back({entry.path().lexically_relative(root)});
    }
    std::ranges::sort(result, [](const AssetEntry& lhs, const AssetEntry& rhs) {
        return lower(lhs.relative.generic_string()) < lower(rhs.relative.generic_string());
    });
    return result;
}

void draw_folder_tree(const std::filesystem::path& folder,
                      const std::vector<AssetEntry>& assets,
                      std::filesystem::path& selectedFolder,
                      std::filesystem::path& selectedAsset) {
    std::set<std::filesystem::path> childFolders;
    for (const auto& asset : assets) {
        auto parent = asset.relative.parent_path();
        while (parent != folder && !parent.empty()) {
            parent = parent.parent_path();
        }
        if (asset.relative.parent_path() != folder && !asset.relative.parent_path().empty()) {
            auto direct = asset.relative.parent_path();
            while (!direct.parent_path().empty() && direct.parent_path() != folder) {
                direct = direct.parent_path();
            }
            if (direct.parent_path() == folder) childFolders.insert(direct);
        }
    }

    for (const auto& child : childFolders) {
        ImGui::PushID(child.generic_string().c_str());
        const bool open = ImGui::TreeNodeEx(child.filename().string().c_str(),
                                            ImGuiTreeNodeFlags_SpanAvailWidth);
        if (ImGui::IsItemClicked()) selectedFolder = child;
        if (open) {
            draw_folder_tree(child, assets, selectedFolder, selectedAsset);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    for (const auto& asset : assets) {
        if (asset.relative.parent_path() != folder) continue;
        ImGui::PushID(asset.relative.generic_string().c_str());
        if (ImGui::Selectable(asset.relative.filename().string().c_str(), selectedAsset == asset.relative)) {
            selectedAsset = asset.relative;
            selectedFolder = folder;
        }
        ImGui::PopID();
    }
}

} // namespace

Engine::Entity AssetManagerPanel::draw(Engine::ScenePreset& scene,
                                        Engine::Assets::Content& content,
                                        const bool disabled) {
    static std::filesystem::path selected;
    static std::filesystem::path selectedFolder;
    static std::string filter;
    static std::vector<AssetEntry> assets;
    static std::filesystem::path scannedRoot;
    static std::string error;
    Engine::Entity created = Engine::NullEntity;

    // Store paths relative to the content root. This is important: Content
    // resolves relative paths against assetRoot(), while the old browser used
    // paths relative to Models/glTF and consequently failed to load them.
    const auto root = content.assetRoot();
    if (root != scannedRoot) {
        scannedRoot = root;
        assets = scan_assets(root);
        selected.clear();
        selectedFolder.clear();
    }

    ImGui::Begin("Asset Manager");
    char filterBuffer[256]{};
    std::snprintf(filterBuffer, sizeof(filterBuffer), "%s", filter.c_str());
    ImGui::SetNextItemWidth(170.0F);
    if (ImGui::InputTextWithHint("##asset-filter", "Search assets...", filterBuffer, sizeof(filterBuffer))) {
        filter = filterBuffer;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) {
        assets = scan_assets(root);
        if (!selected.empty() && std::ranges::none_of(assets, [&](const AssetEntry& asset) {
                return asset.relative == selected;
            })) {
            selected.clear();
            selectedFolder.clear();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu models", assets.size());
    ImGui::BeginChild("##asset-browser", {0.0F, 0.0F}, false);
    ImGui::BeginChild("##asset-tree", {180.0F, 0.0F}, true);
    const auto folderBeforeTree = selectedFolder;
    if (ImGui::Selectable("Assets", selectedFolder.empty())) {
        selectedFolder.clear();
        selected.clear();
        error.clear();
    }
    draw_folder_tree({}, assets, selectedFolder, selected);
    if (selectedFolder != folderBeforeTree) {
        selected.clear();
        error.clear();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##asset-content", {0.0F, 0.0F}, false);
    ImGui::BeginChild("##asset-list", {0.0F, 156.0F}, true);
    constexpr int columns = 3;
    if (ImGui::BeginTable("##asset-grid", columns, ImGuiTableFlags_SizingStretchSame)) {
        for (const auto& asset : assets) {
            if (asset.relative.parent_path() != selectedFolder) continue;
            if (!filter.empty() && lower(asset.relative.generic_string()).find(lower(filter)) == std::string::npos) continue;
            ImGui::TableNextColumn();
            ImGui::PushID(asset.relative.generic_string().c_str());
            const bool isSelected = selected == asset.relative;
            const ImVec2 cardSize{ImGui::GetContentRegionAvail().x - 4.0F, 78.0F};
            if (ImGui::Selectable("##asset-card", isSelected, 0, cardSize)) {
                selected = asset.relative;
                error.clear();
            }
            const ImVec2 cardMin = ImGui::GetItemRectMin();
            const ImVec2 cardMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(
                {cardMin.x + 7.0F, cardMin.y + 8.0F}, {cardMin.x + 38.0F, cardMin.y + 39.0F},
                ImGui::GetColorU32(ImVec4{0.20F, 0.40F, 0.58F, 1.0F}), 4.0F);
            ImGui::GetWindowDrawList()->AddText({cardMin.x + 14.0F, cardMin.y + 13.0F},
                                                 ImGui::GetColorU32(ImVec4{0.85F, 0.95F, 1.0F, 1.0F}), "3D");
            const std::string name = asset.relative.stem().string();
            ImGui::GetWindowDrawList()->AddText({cardMin.x + 7.0F, cardMax.y - 23.0F},
                                                 ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
            ImGui::GetWindowDrawList()->AddText({cardMin.x + 7.0F, cardMax.y - 8.0F},
                                                 ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                 asset.relative.parent_path().generic_string().c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (!selected.empty()) {
        const auto selectedAbsolute = root / selected;
        ImGui::Separator();
        ImGui::TextUnformatted("Selected asset");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", selected.generic_string().c_str());
        if (is_model(selected)) {
            const auto dependencies = gltf_dependencies(selectedAbsolute);
            ImGui::TextDisabled("Dependencies: %zu", dependencies.size());
            for (const auto& dependency : dependencies) {
                std::error_code dependencyError;
                const bool exists = std::filesystem::is_regular_file(dependency, dependencyError);
                ImGui::TextColored(exists ? ImVec4{0.45F, 0.85F, 0.55F, 1.0F}
                                           : ImVec4{0.95F, 0.40F, 0.35F, 1.0F},
                                 "%s %s", exists ? "OK" : "!", dependency.generic_string().c_str());
            }
            ImGui::BeginDisabled(disabled || !error.empty());
            if (ImGui::Button("Add Prefab to Scene", {-1.0F, 0.0F})) {
                try {
                    const auto prefab = Engine::Prefab::model(content, selected);
                    const auto actor = scene.createPrefab(selected.stem().string(), prefab);
                    scene.editor().view<Engine::NameComponent>([&](const Engine::Entity entity,
                                                                    const Engine::NameComponent& name) {
                        if (name.value == actor.name()) created = entity;
                    });
                } catch (const std::exception& exception) {
                    error = exception.what();
                }
            }
            ImGui::EndDisabled();
        } else {
            ImGui::TextDisabled("Select a .gltf or .glb model to instantiate it in the scene.");
        }
    }
    if (!error.empty()) ImGui::TextColored({0.95F, 0.40F, 0.35F, 1.0F}, "%s", error.c_str());
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::End();
    return created;
}
