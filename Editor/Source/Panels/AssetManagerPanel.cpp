#include "Editor/Panels/AssetManagerPanel.h"

#include "Editor/AssetImporter.h"
#include "Editor/Panels/AssetDragDrop.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Editor/Panels/EditorSceneSession.h"
#include "Editor/UI/EditorIcons.h"
#include "Editor/UI/EditorTheme.h"
#include "Editor/UI/EditorWidgets.h"
#include "Elements/NumericControl.h"
#include "Engine/Assets/TextureCooker.h"
#include "Engine/Core/TaskScheduler.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Renderer/ShaderGraph/ShaderGraphSerializer.h"
#include "Engine/Renderer/ShaderGraph/ShaderNodeFactory.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Scene/SceneSerializer.h"
#include "imgui.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <vector>

// NOLINTBEGIN(readability-magic-numbers)
// NOLINTBEGIN(readability-identifier-length)

namespace {
enum class AssetKind { Model, Texture, Shader, Scene, Audio, Other };

struct AssetEntry final {
    std::filesystem::path relative;
    std::uintmax_t size{};
    AssetKind kind{AssetKind::Other};
    // Lower-case generic path, computed once per scan for sorting and the
    // per-frame search filter.
    std::string searchKey;
};

struct AssetCatalog final {
    std::vector<AssetEntry> files;
    std::vector<std::filesystem::path> folders;
};

struct TextureCookJob final {
    std::atomic<bool> running{};
    Engine::Assets::TextureCookProgress progress;
    std::mutex resultMutex;
    std::optional<Engine::Assets::TextureCookSummary> result;
    Engine::TaskHandle task;
};

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

struct ImportDialogState {
    std::mutex mutex;
    bool ready{};
    std::vector<std::filesystem::path> sources;
    std::string error;
    std::filesystem::path destination;
    std::filesystem::path assetRoot;
};

struct ImportResult {
    std::filesystem::path assetRoot;
    std::vector<std::string> imported;
    std::string error;
};

void SDLCALL importDialogFinished(void* userdata, const char* const* files, int) {
    // The dialog owns this reference until its callback, including on shutdown.
    const std::unique_ptr<std::shared_ptr<ImportDialogState>> owner{
        static_cast<std::shared_ptr<ImportDialogState>*>(userdata)};
    const auto& state = *owner;
    std::lock_guard lock(state->mutex);
    try {
        if (!files)
            state->error = SDL_GetError();
        else
            for (auto file = files; *file; ++file) {
                const std::string_view utf8{*file};
                state->sources.emplace_back(std::u8string{utf8.begin(), utf8.end()});
            }
    } catch (const std::exception& exception) {
        state->error = exception.what();
    }
    state->ready = true;
}

void showImportDialog(const std::shared_ptr<ImportDialogState>& state, const bool folder, const bool archiveOnly) {
    static constexpr SDL_DialogFileFilter archiveFilter{"ZIP archives", "zip;ZIP"};
    static constexpr SDL_DialogFileFilter allFilesFilter{"All files", "*"};
    auto* callbackState = new std::shared_ptr<ImportDialogState>{state};
    // The focused window belongs to this Editor; SDL supports portal dialogs on Wayland.
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (folder) {
        SDL_ShowOpenFolderDialog(importDialogFinished, callbackState, window, nullptr, true);
    } else {
        SDL_ShowOpenFileDialog(importDialogFinished, callbackState, window,
                               archiveOnly ? &archiveFilter : &allFilesFilter, 1, nullptr, true);
    }
}

bool is_model(const std::filesystem::path& path) {
    const auto ext = lower(path.extension().string());
    return ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".gmesh";
}

bool is_shader_graph(const std::filesystem::path& path) {
    return lower(path.extension().string()) == ".shadergraph";
}

AssetKind asset_kind(const std::filesystem::path& path) {
    const auto ext = lower(path.extension().string());
    if (ext == ".gltf" || ext == ".glb" || ext == ".gmesh" || ext == ".obj" || ext == ".fbx")
        return AssetKind::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".dds" ||
        ext == ".hdr")
        return AssetKind::Texture;
    if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".glsl" || ext == ".slang" || ext == ".spv" ||
        ext == ".shadergraph")
        return AssetKind::Shader;
    if (ext == ".scene")
        return AssetKind::Scene;
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3")
        return AssetKind::Audio;
    return AssetKind::Other;
}

const char* kind_name(const AssetKind kind) {
    switch (kind) {
    case AssetKind::Model:
        return "3D Model";
    case AssetKind::Texture:
        return "Texture";
    case AssetKind::Shader:
        return "Shader";
    case AssetKind::Scene:
        return "Scene";
    case AssetKind::Audio:
        return "Audio";
    case AssetKind::Other:
        return "File";
    }
    return "File";
}

ImVec4 kind_color(const AssetKind kind) {
    switch (kind) {
    case AssetKind::Model:
        return {0.25F, 0.72F, 0.88F, 1.0F};
    case AssetKind::Texture:
        return {0.69F, 0.55F, 0.91F, 1.0F};
    case AssetKind::Shader:
        return {0.95F, 0.65F, 0.27F, 1.0F};
    case AssetKind::Scene:
        return {0.35F, 0.82F, 0.59F, 1.0F};
    case AssetKind::Audio:
        return {0.91F, 0.45F, 0.62F, 1.0F};
    case AssetKind::Other:
        return {0.54F, 0.60F, 0.69F, 1.0F};
    }
    return {0.54F, 0.60F, 0.69F, 1.0F};
}

std::string decode_uri(std::string value) {
    const auto hex = [](const char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 2 < value.size();) {
        if (value[i] != '%') {
            ++i;
            continue;
        }
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
    if (lower(model.extension().string()) != ".gltf")
        return result;
    std::ifstream file(model);
    if (!file)
        return result;
    const std::string source{std::istreambuf_iterator<char>{file}, {}};
    const std::regex uriPattern{R"REGEX("uri"\s*:\s*"([^"]+)")REGEX"};
    for (std::sregex_iterator it{source.begin(), source.end(), uriPattern}, end; it != end; ++it) {
        std::string uri = decode_uri((*it)[1].str());
        if (uri.starts_with("data:") || uri.starts_with("#"))
            continue;
        if (const auto query = uri.find_first_of("?#"); query != std::string::npos)
            uri.erase(query);
        const auto path = (model.parent_path() / std::filesystem::path{uri}).lexically_normal();
        if (std::ranges::find(result, path) == result.end())
            result.push_back(path);
    }
    return result;
}

AssetCatalog scan_assets(const std::filesystem::path& root) {
    AssetCatalog result;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error))
        return result;
    for (std::filesystem::recursive_directory_iterator
             it{root, std::filesystem::directory_options::skip_permission_denied, error},
         end;
         it != end; it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        const auto relative = it->path().lexically_relative(root);
        if (it->is_directory(error))
            result.folders.push_back(relative);
        else if (it->is_regular_file(error))
            result.files.push_back(
                {relative, it->file_size(error), asset_kind(relative), lower(relative.generic_string())});
        error.clear();
    }
    std::ranges::sort(result.files, {}, &AssetEntry::searchKey);
    std::vector<std::pair<std::string, std::filesystem::path>> folders;
    folders.reserve(result.folders.size());
    for (auto& folder : result.folders)
        folders.emplace_back(lower(folder.generic_string()), std::move(folder));
    std::ranges::sort(folders, {}, &std::pair<std::string, std::filesystem::path>::first);
    for (std::size_t index = 0; index < folders.size(); ++index)
        result.folders[index] = std::move(folders[index].second);
    return result;
}

std::string format_size(const std::uintmax_t bytes) {
    char text[32]{};
    if (bytes >= 1024ULL * 1024ULL)
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024ULL)
        std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    return text;
}

std::string clipped_label(const std::string& value, const float width) {
    if (ImGui::CalcTextSize(value.c_str()).x <= width)
        return value;
    std::string result = value;
    while (result.size() > 3 && ImGui::CalcTextSize((result + "...").c_str()).x > width)
        result.pop_back();
    return result + "...";
}

bool scene_uses_asset(const Engine::ScenePreset& scene, const std::filesystem::path& root,
                      const std::filesystem::path& relative) {
    const auto assetPath = (root / relative).lexically_normal();
    bool used = false;
    scene.view().view<Engine::MeshRenderer>([&](const Engine::Entity, const Engine::MeshRenderer& renderer) {
        const auto resource = renderer.mesh.resource();
        if (resource && resource->sourcePath.lexically_normal() == assetPath)
            used = true;
    });
    return used;
}

bool delete_asset_file(const Engine::ScenePreset& scene, const std::filesystem::path& root,
                       const std::filesystem::path& relative, std::string& error) {
    // Asset paths come from the catalog, but keep this check here as a hard
    // boundary before performing a filesystem mutation.
    if (relative.empty() || relative.is_absolute()) {
        error = "Invalid asset path";
        return false;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            error = "Asset path escapes the asset directory";
            return false;
        }
    }

    if (scene_uses_asset(scene, root, relative)) {
        error = "Asset is used by the current scene; remove it from the scene first";
        return false;
    }

    const auto path = (root / relative).lexically_normal();
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(path, filesystemError)) {
        error = "Asset does not exist: " + relative.generic_string();
        return false;
    }
    if (!std::filesystem::remove(path, filesystemError) || filesystemError) {
        error = "Could not delete asset: " + relative.generic_string();
        return false;
    }
    if (path.extension() == ".scene")
        Engine::SceneSerializer::removeSidecars(path);
    return true;
}

// Relative to the asset root, non-empty, and never escaping it.
bool valid_relative_asset_path(const std::filesystem::path& relative, std::string& error) {
    if (relative.empty() || relative.is_absolute()) {
        error = "Invalid asset path";
        return false;
    }
    if (std::ranges::any_of(relative, [](const std::filesystem::path& part) { return part == ".."; })) {
        error = "Asset path escapes the asset directory";
        return false;
    }
    return true;
}

// A single file or folder name: no separators, not "." or "..".
bool valid_asset_name(const std::string& name) {
    const std::filesystem::path path{name};
    return !name.empty() && path.filename() == path && name != "." && name != ".." &&
           name.find_first_of("/\\") == std::string::npos;
}

// True when a mesh of the scene comes from @p relative or from inside it.
bool scene_uses_asset_or_folder(const Engine::ScenePreset& scene, const std::filesystem::path& root,
                                const std::filesystem::path& relative) {
    const auto target = (root / relative).lexically_normal();
    bool used = false;
    scene.view().view<Engine::MeshRenderer>([&](const Engine::Entity, const Engine::MeshRenderer& renderer) {
        const auto resource = renderer.mesh.resource();
        if (!resource || resource->sourcePath.empty())
            return;
        const auto source = resource->sourcePath.lexically_normal();
        const auto [end, unused] = std::mismatch(target.begin(), target.end(), source.begin(), source.end());
        static_cast<void>(unused);
        if (end == target.end())
            used = true;
    });
    return used;
}

// Sidecars written next to a .scene (SceneSerializer::removeSidecars).
constexpr std::array<const char*, 6> SceneSidecarSuffixes{".terrain", ".environment", ".gi",
                                                          ".post",    ".surface",     ".fog"};

bool delete_asset_folder(const Engine::ScenePreset& scene, const std::filesystem::path& root,
                         const std::filesystem::path& relative, std::vector<std::filesystem::path>& deletedScenes,
                         std::string& error) {
    if (!valid_relative_asset_path(relative, error))
        return false;
    if (scene_uses_asset_or_folder(scene, root, relative)) {
        error = "The current scene uses assets in this folder; remove them from the scene first";
        return false;
    }
    const auto path = (root / relative).lexically_normal();
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(path, filesystemError)) {
        error = "Folder does not exist: " + relative.generic_string();
        return false;
    }
    for (std::filesystem::recursive_directory_iterator it{path, filesystemError}, end; !filesystemError && it != end;
         it.increment(filesystemError))
        if (it->is_regular_file() && it->path().extension() == ".scene")
            deletedScenes.push_back(it->path().lexically_normal());
    std::filesystem::remove_all(path, filesystemError);
    if (filesystemError) {
        error = "Could not delete folder: " + relative.generic_string() + " (" + filesystemError.message() + ")";
        return false;
    }
    return true;
}

bool rename_asset(const Engine::ScenePreset& scene, const std::filesystem::path& root,
                  const std::filesystem::path& relative, const std::string& newName, std::filesystem::path& renamed,
                  std::string& error) {
    if (!valid_relative_asset_path(relative, error))
        return false;
    if (!valid_asset_name(newName)) {
        error = "Enter a valid name";
        return false;
    }
    // Scene files reference mesh assets by absolute path.
    if (scene_uses_asset_or_folder(scene, root, relative)) {
        error = "The current scene uses this asset; remove it from the scene before renaming";
        return false;
    }
    const auto from = (root / relative).lexically_normal();
    renamed = relative.parent_path() / newName;
    const auto to = (root / renamed).lexically_normal();
    std::error_code filesystemError;
    if (std::filesystem::exists(to, filesystemError)) {
        error = "'" + newName + "' already exists";
        return false;
    }
    std::filesystem::rename(from, to, filesystemError);
    if (filesystemError) {
        error = "Could not rename '" + relative.generic_string() + "': " + filesystemError.message();
        return false;
    }
    if (from.extension() == ".scene" && to.extension() == ".scene")
        for (const char* suffix : SceneSidecarSuffixes) {
            const std::filesystem::path sidecar{from.string() + suffix};
            if (std::filesystem::exists(sidecar, filesystemError))
                std::filesystem::rename(sidecar, std::filesystem::path{to.string() + suffix}, filesystemError);
        }
    return true;
}

bool duplicate_asset(const std::filesystem::path& root, const std::filesystem::path& relative,
                     std::filesystem::path& copy, std::string& error) {
    if (!valid_relative_asset_path(relative, error))
        return false;
    const auto source = (root / relative).lexically_normal();
    const bool folder = std::filesystem::is_directory(source);
    const std::string stem = folder ? relative.filename().string() : relative.stem().string();
    const std::string extension = folder ? std::string{} : relative.extension().string();
    copy = relative.parent_path() / (stem + " Copy" + extension);
    for (unsigned int index = 2; std::filesystem::exists(root / copy); ++index)
        copy = relative.parent_path() / (stem + " Copy " + std::to_string(index) + extension);
    std::error_code filesystemError;
    std::filesystem::copy(source, root / copy,
                          folder ? std::filesystem::copy_options::recursive : std::filesystem::copy_options::none,
                          filesystemError);
    if (!filesystemError && source.extension() == ".scene")
        for (const char* suffix : SceneSidecarSuffixes) {
            const std::filesystem::path sidecar{source.string() + suffix};
            if (std::filesystem::exists(sidecar))
                std::filesystem::copy_file(sidecar, std::filesystem::path{(root / copy).string() + suffix},
                                           filesystemError);
        }
    if (filesystemError) {
        error = "Could not duplicate '" + relative.generic_string() + "': " + filesystemError.message();
        return false;
    }
    return true;
}

void draw_folder_icon(const ImVec2 min, const float size, const bool selected) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec4 base = selected ? EditorUI::colors().accent : EditorUI::colors().warning;
    const ImU32 back = ImGui::GetColorU32(ImVec4{base.x * 0.78F, base.y * 0.78F, base.z * 0.78F, 1.0F});
    const ImU32 front = ImGui::GetColorU32(base);
    const float y = min.y + size * 0.16F;
    draw->AddRectFilled({min.x, y + size * 0.14F}, {min.x + size, y + size * 0.75F}, back, size * 0.08F);
    draw->AddRectFilled({min.x + size * 0.06F, y}, {min.x + size * 0.48F, y + size * 0.29F}, back, size * 0.06F);
    draw->AddRectFilled({min.x, y + size * 0.27F}, {min.x + size, y + size * 0.82F}, front, size * 0.08F);
}

EditorUI::Icon kind_icon(const AssetKind kind) {
    switch (kind) {
    case AssetKind::Model:
        return EditorUI::Icon::Mesh;
    case AssetKind::Texture:
        return EditorUI::Icon::Image;
    case AssetKind::Shader:
        return EditorUI::Icon::Code;
    case AssetKind::Scene:
        return EditorUI::Icon::Layers;
    case AssetKind::Audio:
        return EditorUI::Icon::Audio;
    case AssetKind::Other:
        return EditorUI::Icon::File;
    }
    return EditorUI::Icon::File;
}

void draw_file_icon(const ImVec2 min, const float size, const AssetKind kind, const bool selected) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec4 accent = selected ? EditorUI::colors().accentHover : kind_color(kind);
    const ImU32 color = ImGui::GetColorU32(accent);
    const ImU32 fill = ImGui::GetColorU32(ImVec4{accent.x * 0.2F, accent.y * 0.2F, accent.z * 0.2F, 1.0F});
    const float fold = size * 0.25F;
    // Page with a folded corner, the asset type icon in its centre.
    const std::array page{min, ImVec2{min.x + size - fold, min.y}, ImVec2{min.x + size, min.y + fold},
                          ImVec2{min.x + size, min.y + size}, ImVec2{min.x, min.y + size}};
    draw->AddConvexPolyFilled(page.data(), static_cast<int>(page.size()), fill);
    draw->AddPolyline(page.data(), static_cast<int>(page.size()), color, ImDrawFlags_Closed, 1.5F);
    draw->AddLine({min.x + size - fold, min.y}, {min.x + size - fold, min.y + fold}, color, 1.3F);
    draw->AddLine({min.x + size - fold, min.y + fold}, {min.x + size, min.y + fold}, color, 1.3F);
    EditorUI::drawIcon(draw, kind_icon(kind), {min.x + size * 0.5F, min.y + size * 0.56F}, size * 0.44F, color);
}

bool matches_filter(const AssetEntry& asset, const std::string& filter) {
    return filter.empty() || asset.searchKey.find(filter) != std::string::npos ||
           lower(kind_name(asset.kind)).find(filter) != std::string::npos;
}

void draw_folder_tree(const std::filesystem::path& parent, const std::vector<std::filesystem::path>& folders,
                      std::filesystem::path& selectedFolder, std::filesystem::path& selectedAsset,
                      const std::function<void(const std::filesystem::path&)>& contextMenu) {
    for (const auto& folder : folders) {
        if (folder.parent_path() != parent)
            continue;
        ImGui::PushID(folder.generic_string().c_str());
        auto flags =
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (selectedFolder == folder)
            flags |= ImGuiTreeNodeFlags_Selected;
        // Leading spaces reserve room for the folder icon drawn after the arrow.
        const std::string label = "     " + folder.filename().string();
        bool hasChildren = false;
        for (const auto& candidate : folders)
            if (candidate.parent_path() == folder) {
                hasChildren = true;
                break;
            }
        if (!hasChildren)
            flags |= ImGuiTreeNodeFlags_Leaf;
        const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            selectedFolder = folder;
            selectedAsset.clear();
        }
        {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const float iconSize = ImGui::GetFontSize() * 0.9F;
            const float x = min.x + ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2.0F;
            EditorUI::drawIcon(
                ImGui::GetWindowDrawList(), EditorUI::Icon::Folder, {x + iconSize * 0.5F, (min.y + max.y) * 0.5F},
                iconSize,
                ImGui::GetColorU32(selectedFolder == folder ? EditorUI::colors().accent : EditorUI::colors().warning));
        }
        if (ImGui::BeginPopupContextItem("##folder-tree-context")) {
            contextMenu(folder);
            ImGui::EndPopup();
        }
        if (open) {
            draw_folder_tree(folder, folders, selectedFolder, selectedAsset, contextMenu);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void draw_breadcrumbs(std::filesystem::path& folder, std::filesystem::path& selectedAsset) {
    if (ImGui::SmallButton("Assets")) {
        folder.clear();
        selectedAsset.clear();
    }
    // A breadcrumb click assigns to `folder`. Iterate a stable snapshot: a
    // range-for over `folder` itself would retain invalid path iterators after
    // that assignment and crash while advancing to the next segment.
    const std::filesystem::path displayedFolder = folder;
    std::filesystem::path accumulated;
    for (const auto& part : displayedFolder) {
        accumulated /= part;
        ImGui::SameLine(0.0F, 2.0F);
        EditorUI::iconLabel(EditorUI::Icon::ChevronRight, EditorUI::colors().textSecondary);
        ImGui::SameLine(0.0F, 2.0F);
        ImGui::PushID(accumulated.generic_string().c_str());
        if (ImGui::SmallButton(part.string().c_str())) {
            folder = accumulated;
            selectedAsset.clear();
        }
        ImGui::PopID();
    }
}
} // namespace

namespace {
AssetManagerPanel::Request pendingRequest = AssetManagerPanel::Request::None;
}

void AssetManagerPanel::request(const Request request) {
    pendingRequest = request;
}

Engine::Entity AssetManagerPanel::draw(Engine::ScenePreset& scene, Engine::Assets::Content& content,
                                       const bool disabled, bool& isOpen, const bool projectIsOpen,
                                       const std::function<void(const std::filesystem::path&)>& openShaderGraph,
                                       const std::function<void(const std::filesystem::path&)>& sceneDeleted) {
    static std::filesystem::path selected;
    static std::filesystem::path selectedFolder;
    static std::filesystem::path scannedRoot;
    static std::string filter;
    static std::string error;
    static AssetCatalog catalog;
    static float tileSize = 92.0F;
    static bool gridView = true;
    static bool showInspector = true;
    static std::filesystem::path deleteCandidate;
    static TextureCookJob cookJob;
    static std::shared_ptr<ImportDialogState> importDialog;
    static std::optional<std::future<ImportResult>> importJob;
    static int createKind = 0; // 1 = folder, 2 = file
    static std::string createName;
    static bool focusCreateName = false;
    // Folder the create dialog writes into: the one right-clicked, or the
    // open folder for the toolbar's + button.
    static std::filesystem::path createParent;
    static std::filesystem::path renameSource;
    static std::string renameName;
    static bool focusRenameName = false;
    bool openDeletePopup = false;
    bool openCreatePopup = false;
    bool openRenamePopup = false;
    Engine::Entity created = Engine::NullEntity;

    const auto root = content.assetRoot();
    if (root != scannedRoot) {
        scannedRoot = root;
        catalog = scan_assets(root);
        selected.clear();
        selectedFolder.clear();
        error.clear();
    }
    const auto refresh = [&] {
        catalog = scan_assets(root);
        if (!selected.empty() &&
            std::ranges::none_of(catalog.files, [&](const AssetEntry& a) { return a.relative == selected; }))
            selected.clear();
        if (!selectedFolder.empty() && std::ranges::find(catalog.folders, selectedFolder) == catalog.folders.end())
            selectedFolder.clear();
    };
    if (importDialog) {
        std::lock_guard lock(importDialog->mutex);
        if (importDialog->ready) {
            if (!importDialog->error.empty()) {
                error = importDialog->error;
                Editor::ConsolePanel::error("Could not open import dialog: " + error);
            } else if (!importDialog->sources.empty()) {
                const auto state = importDialog;
                importJob.emplace(std::async(std::launch::async, [state] {
                    ImportResult result{.assetRoot = state->assetRoot};
                    for (const auto& source : state->sources) {
                        std::string failure;
                        if (Editor::AssetImporter::import(source, state->destination, failure))
                            result.imported.push_back(source.filename().string());
                        else {
                            if (!result.error.empty())
                                result.error += "\n";
                            result.error += source.filename().string() + ": " + failure;
                        }
                    }
                    return result;
                }));
            }
        }
    }
    // Release the dialog state after dropping its lock; a callback may have
    // been delivered synchronously by the platform.
    if (importDialog) {
        bool ready;
        {
            std::lock_guard lock(importDialog->mutex);
            ready = importDialog->ready;
        }
        if (ready)
            importDialog.reset();
    }
    if (importJob && importJob->wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        try {
            const auto result = importJob->get();
            if (result.assetRoot == root)
                refresh();
            for (const auto& name : result.imported)
                Editor::ConsolePanel::info("Imported asset: " + name);
            if (!result.error.empty()) {
                error = result.error;
                Editor::ConsolePanel::error("Could not import asset: " + error);
            } else
                error.clear();
        } catch (const std::exception& exception) {
            error = exception.what();
            Editor::ConsolePanel::error("Could not import asset: " + error);
        }
        importJob.reset();
    }
    const auto instantiate = [&] {
        if (selected.empty() || !is_model(selected))
            return;
        try {
            Editor::AssetDragDrop::requestModel(content, selected);
            error.clear();
        } catch (const std::exception& exception) {
            error = exception.what();
        }
    };
    const auto requestDelete = [&](const std::filesystem::path& asset) {
        deleteCandidate = asset;
        openDeletePopup = true;
    };
    const auto createShaderGraph = [&](const std::filesystem::path& parent) {
        std::filesystem::path relative = parent / "New Shader Graph.shadergraph";
        for (unsigned int index = 2; std::filesystem::exists(root / relative); ++index)
            relative = parent / ("New Shader Graph " + std::to_string(index) + ".shadergraph");
        try {
            Engine::ShaderGraphAsset graph{.id = Engine::createUUID(), .name = relative.stem().string()};
            Engine::ShaderPinId nextPin = 1;
            graph.nodes.push_back(Engine::ShaderNodeFactory::create(Engine::ShaderNodeType::SurfaceOutput, 1, nextPin));
            Engine::ShaderGraphSerializer::save(graph, root / relative);
            refresh();
            selected = relative;
            openShaderGraph(relative);
        } catch (const std::exception& exception) {
            error = exception.what();
        }
    };
    // Dialogs open at panel level after the context menu that asked for them
    // has closed (OpenPopup must match BeginPopup's ID stack).
    const auto requestCreate = [&](const int kind, const std::filesystem::path& parent) {
        createKind = kind;
        createParent = parent;
        createName = kind == 1 ? "New Folder" : "NewFile.txt";
        openCreatePopup = true;
    };
    const auto requestRename = [&](const std::filesystem::path& relative) {
        renameSource = relative;
        renameName = relative.filename().string();
        openRenamePopup = true;
    };
    const auto requestImport = [&](const std::filesystem::path& folder, const bool importFolder,
                                   const bool archiveOnly) {
        if (importDialog || importJob)
            return;
        importDialog = std::make_shared<ImportDialogState>();
        importDialog->destination = root / folder;
        importDialog->assetRoot = root;
        showImportDialog(importDialog, importFolder, archiveOnly);
    };
    const auto duplicate = [&](const std::filesystem::path& relative) {
        std::filesystem::path copy;
        if (duplicate_asset(root, relative, copy, error)) {
            Editor::ConsolePanel::info("Duplicated " + relative.generic_string() + " as " + copy.generic_string());
            error.clear();
            refresh();
            if (std::filesystem::is_regular_file(root / copy))
                selected = copy;
        } else {
            Editor::ConsolePanel::error(error);
        }
    };
    const auto showInFileManager = [&](const std::filesystem::path& relative) {
        const auto path = (root / relative).lexically_normal();
        const auto folder = std::filesystem::is_directory(path) ? path : path.parent_path();
        if (!SDL_OpenURL(("file://" + folder.string()).c_str()))
            Editor::ConsolePanel::error(std::string{"Could not open the file manager: "} + SDL_GetError());
    };
    const auto copyPath = [&](const std::filesystem::path& relative) {
        ImGui::SetClipboardText((root / relative).lexically_normal().string().c_str());
    };
    // Create / Import entries shared by every context menu.
    const auto drawCreateItems = [&](const std::filesystem::path& parent) {
        if (ImGui::BeginMenu("Create")) {
            if (ImGui::MenuItem("Folder"))
                requestCreate(1, parent);
            if (ImGui::MenuItem("File"))
                requestCreate(2, parent);
            ImGui::Separator();
            if (ImGui::MenuItem("Shader Graph"))
                createShaderGraph(parent);
            ImGui::EndMenu();
        }
        ImGui::BeginDisabled(importDialog || importJob);
        if (ImGui::BeginMenu("Import")) {
            if (ImGui::MenuItem("File..."))
                requestImport(parent, false, false);
            if (ImGui::MenuItem("Folder..."))
                requestImport(parent, true, false);
            if (ImGui::MenuItem("ZIP archive..."))
                requestImport(parent, false, true);
            ImGui::EndMenu();
        }
        ImGui::EndDisabled();
    };
    const auto drawPathItems = [&](const std::filesystem::path& relative) {
        if (ImGui::MenuItem("Show in file manager"))
            showInFileManager(relative);
        if (ImGui::MenuItem("Copy path"))
            copyPath(relative);
    };
    // Right-click on a folder (tree, grid or list).
    const auto drawFolderMenu = [&](const std::filesystem::path& folder) {
        ImGui::TextDisabled("%s", folder.filename().string().c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Open")) {
            selectedFolder = folder;
            selected.clear();
        }
        drawCreateItems(folder);
        ImGui::Separator();
        if (ImGui::MenuItem("Rename..."))
            requestRename(folder);
        if (ImGui::MenuItem("Duplicate"))
            duplicate(folder);
        if (ImGui::MenuItem("Delete folder..."))
            requestDelete(folder);
        ImGui::Separator();
        drawPathItems(folder);
    };
    // Right-click on empty space: create and import into @p folder.
    const auto drawBackgroundMenu = [&](const std::filesystem::path& folder) {
        ImGui::TextDisabled("%s", folder.empty() ? "Assets" : ("Assets/" + folder.generic_string()).c_str());
        ImGui::Separator();
        drawCreateItems(folder);
        ImGui::Separator();
        if (ImGui::MenuItem("Show in file manager"))
            showInFileManager(folder);
        if (ImGui::MenuItem("Refresh"))
            refresh();
    };
    // Entries for a right-clicked file, after its own specific actions.
    const auto drawFileItems = [&](const std::filesystem::path& relative) {
        if (ImGui::MenuItem("Rename...")) {
            selected = relative;
            requestRename(relative);
        }
        if (ImGui::MenuItem("Duplicate"))
            duplicate(relative);
        if (ImGui::MenuItem("Delete asset...")) {
            selected = relative;
            requestDelete(relative);
        }
        ImGui::Separator();
        drawPathItems(relative);
    };

    if (cookJob.task && cookJob.task.ready()) {
        std::optional<Engine::Assets::TextureCookSummary> summary;
        {
            std::scoped_lock lock(cookJob.resultMutex);
            summary = std::move(cookJob.result);
        }
        cookJob.task = {};
        cookJob.running.store(false, std::memory_order_release);
        if (summary) {
            refresh();
            const auto ms = [](const double duration) {
                char buffer[32]{};
                std::snprintf(buffer, sizeof buffer, "%.1f", duration);
                return std::string{buffer};
            };
            const auto report = "Cooked " + std::to_string(summary->cooked) + " assets; skipped " +
                                std::to_string(summary->skipped) + " of " + std::to_string(summary->discovered) +
                                ".\nTextures: source hash " + ms(summary->standalone.sourceHashMilliseconds) +
                                " ms, decode " + ms(summary->standalone.decodeMilliseconds) + " ms, mips " +
                                ms(summary->standalone.mipGenerationMilliseconds) + " ms, encode " +
                                ms(summary->standalone.blockEncodeMilliseconds) + " ms, .gtex write " +
                                ms(summary->standalone.writeMilliseconds) + " ms.\nglTF: source scan " +
                                ms(summary->gltf.sourceScanMilliseconds) + " ms, parse " +
                                ms(summary->gltf.parseMilliseconds) + " ms, image decode " +
                                ms(summary->gltfTextures.decodeMilliseconds) + " ms, image hash " +
                                ms(summary->gltfTextures.sourceHashMilliseconds) + " ms, geometry " +
                                ms(summary->gltf.geometryImportMilliseconds) + " ms, meshlets " +
                                ms(summary->gltf.meshletBuildMilliseconds) + " ms, mips " +
                                ms(summary->gltfTextures.mipGenerationMilliseconds) + " ms, encode " +
                                ms(summary->gltfTextures.blockEncodeMilliseconds) + " ms, .gtex write " +
                                ms(summary->gltfTextures.writeMilliseconds) + " ms, .gmesh write " +
                                ms(summary->gltf.meshWriteMilliseconds) + " ms.";
            if (summary->failed == 0) {
                Editor::ConsolePanel::info(report);
            } else {
                Editor::ConsolePanel::error(report + "\n" + std::to_string(summary->failed) + " failed.\n" +
                                            summary->errors);
            }
        }
    }

    if (const Request queued = std::exchange(pendingRequest, Request::None); queued != Request::None && projectIsOpen) {
        if (queued == Request::CreateShaderGraph) {
            createShaderGraph(selectedFolder);
        } else if (!importDialog && !importJob) {
            importDialog = std::make_shared<ImportDialogState>();
            importDialog->destination = root / selectedFolder;
            importDialog->assetRoot = root;
            showImportDialog(importDialog, queued == Request::ImportFolder, queued == Request::ImportArchive);
        }
    }

    ImGui::Begin("Asset Manager", &isOpen);
    // Toolbar: navigation | create & import | search | view options.
    ImGui::BeginDisabled(selectedFolder.empty());
    if (EditorUI::iconButton("##asset-up", EditorUI::Icon::Back, "Go to parent folder")) {
        selectedFolder = selectedFolder.parent_path();
        selected.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0F, 2.0F);
    if (EditorUI::iconButton("##asset-refresh", EditorUI::Icon::Refresh, "Refresh assets"))
        refresh();
    EditorUI::toolbarSeparator();

    ImGui::BeginDisabled(!projectIsOpen || importDialog || importJob);
    if (EditorUI::iconButton("##asset-import", EditorUI::Icon::Import, "Import files, a folder or a ZIP archive"))
        ImGui::OpenPopup("##asset-import-menu");
    if (ImGui::BeginPopup("##asset-import-menu")) {
        if (ImGui::MenuItem("File..."))
            requestImport(selectedFolder, false, false);
        if (ImGui::MenuItem("Folder..."))
            requestImport(selectedFolder, true, false);
        if (ImGui::MenuItem("ZIP archive..."))
            requestImport(selectedFolder, false, true);
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0F, 2.0F);
    ImGui::BeginDisabled(!projectIsOpen);
    if (EditorUI::iconButton("##asset-create", EditorUI::Icon::Plus, "Create a new asset"))
        ImGui::OpenPopup("##asset-create-menu");
    if (ImGui::BeginPopup("##asset-create-menu")) {
        if (ImGui::MenuItem("Folder"))
            requestCreate(1, selectedFolder);
        if (ImGui::MenuItem("File"))
            requestCreate(2, selectedFolder);
        ImGui::Separator();
        if (ImGui::MenuItem("Shader Graph"))
            createShaderGraph(selectedFolder);
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0F, 2.0F);
    ImGui::BeginDisabled(!projectIsOpen || cookJob.running.load(std::memory_order_acquire));
    if (EditorUI::iconButton("##asset-compress", EditorUI::Icon::Archive,
                             "Compress all textures and cook models (.gtex / .gmesh)"))
        ImGui::OpenPopup("##asset-compress-confirm");
    if (EditorUI::beginDialog("##asset-compress-confirm", 440.0F)) {
        EditorUI::dialogHeader(EditorUI::Icon::Archive, EditorUI::colors().accent, "Compress project assets",
                               "Generates .gtex files for source images and .gmesh files for every FBX/GLB/glTF "
                               "in Assets, with their textures. Compressed DDS with mipmaps are copied as is.");
        ImGui::TextDisabled("Unchanged cooked assets are skipped. This can take a while.");
        EditorUI::dialogFooter(2, 120.0F);
        if (ImGui::Button("Cancel", {120.0F, 0.0F}) || EditorUI::dialogEscapePressed())
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (EditorUI::primaryButton("Compress all", {120.0F, 0.0F})) {
            cookJob.progress.reset();
            {
                std::scoped_lock lock(cookJob.resultMutex);
                cookJob.result.reset();
            }
            cookJob.running.store(true, std::memory_order_release);
            auto* job = &cookJob;
            cookJob.task = Engine::TaskScheduler::global().schedule(
                [job, root] {
                    Engine::Assets::TextureCookSummary summary;
                    try {
                        summary = Engine::Assets::cook_all_textures(root, &job->progress);
                        const auto meshes = Engine::Assets::cook_all_gltf_meshes(root, &job->progress);
                        summary.discovered += meshes.discovered;
                        summary.cooked += meshes.cooked;
                        summary.skipped += meshes.skipped;
                        summary.failed += meshes.failed;
                        summary.errors += meshes.errors;
                        summary.gltfTextures = meshes.gltfTextures;
                        summary.gltf = meshes.gltf;
                    } catch (const std::exception& exception) {
                        summary.failed = 1;
                        summary.errors = exception.what();
                    } catch (...) {
                        summary.failed = 1;
                        summary.errors = "Unknown error while cooking textures";
                    }
                    {
                        std::scoped_lock lock(job->resultMutex);
                        job->result = std::move(summary);
                    }
                },
                Engine::TaskPriority::Low);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    EditorUI::toolbarSeparator();

    // View options are right-aligned; the search box takes the space between.
    const float viewOptionsWidth = ImGui::GetFrameHeight() * 3.0F + 82.0F + ImGui::GetStyle().ItemSpacing.x * 3.0F;
    char filterBuffer[256]{};
    std::snprintf(filterBuffer, sizeof(filterBuffer), "%s", filter.c_str());
    if (EditorUI::searchBox("##asset-filter", "Search all assets...", filterBuffer, sizeof(filterBuffer),
                            std::max(120.0F, ImGui::GetContentRegionAvail().x - viewOptionsWidth)))
        filter = filterBuffer;
    ImGui::SameLine();
    ImGui::BeginDisabled(!gridView);
    ImGui::SetNextItemWidth(82.0F);
    Editor::Controls::sliderFloat("##asset-size", &tileSize, 72.0F, 136.0F, "");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Tile size");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorUI::iconButton("##asset-grid-view", EditorUI::Icon::Grid, "Grid view", {.active = gridView}))
        gridView = true;
    ImGui::SameLine(0.0F, 2.0F);
    if (EditorUI::iconButton("##asset-list-view", EditorUI::Icon::List, "List view", {.active = !gridView}))
        gridView = false;
    ImGui::SameLine(0.0F, 2.0F);
    if (EditorUI::iconButton("##asset-details", EditorUI::Icon::Sidebar,
                             showInspector ? "Hide asset details" : "Show asset details", {.active = showInspector}))
        showInspector = !showInspector;

    // Background work gets its own row so the toolbar layout never jumps.
    if (cookJob.running.load(std::memory_order_acquire)) {
        const auto discovered = cookJob.progress.discovered.load(std::memory_order_acquire);
        const auto completed = cookJob.progress.completed.load(std::memory_order_acquire);
        const bool cookPaused = cookJob.progress.paused.load(std::memory_order_acquire);
        if (EditorUI::iconButton("##asset-cook-pause", cookPaused ? EditorUI::Icon::Play : EditorUI::Icon::Pause,
                                 cookPaused ? "Resume compression" : "Pause compression"))
            cookJob.progress.paused.store(!cookJob.progress.paused.load(std::memory_order_relaxed),
                                          std::memory_order_release);
        ImGui::SameLine();
        // Textures inside models (DDS, embedded images) are counted as each
        // model is read, so the total grows during the model phase.
        const auto models = cookJob.progress.modelsDiscovered.load(std::memory_order_acquire);
        const auto modelsDone = cookJob.progress.modelsCompleted.load(std::memory_order_acquire);
        const std::string current = cookJob.progress.current();
        char overlay[384]{};
        if (models == 0)
            std::snprintf(overlay, sizeof(overlay), "%s textures  %u / %u%s%s", cookPaused ? "Paused" : "Compressing",
                          completed, discovered, current.empty() ? "" : "  -  ", current.c_str());
        else
            std::snprintf(overlay, sizeof(overlay), "%s  model %u / %u  -  textures %u / %u%s%s",
                          cookPaused ? "Paused" : "Compressing", std::min(modelsDone + 1, models), models, completed,
                          discovered, current.empty() ? "" : "  -  ", current.c_str());
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                              cookPaused ? EditorUI::colors().warning : EditorUI::colors().accent);
        ImGui::ProgressBar(discovered == 0 ? 0.0F : static_cast<float>(completed) / static_cast<float>(discovered),
                           {-1.0F, ImGui::GetFrameHeight()}, overlay);
        ImGui::PopStyleColor();
    }
    if (importDialog || importJob) {
        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()) * 0.6F, {-1.0F, ImGui::GetFrameHeight()},
                           importDialog ? "Selecting assets..." : "Importing assets...");
    }

    const std::string filterLower = lower(filter);
    const auto visibleFiles = std::ranges::count_if(catalog.files, [&](const AssetEntry& a) {
        return matches_filter(a, filterLower) && (!filter.empty() || a.relative.parent_path() == selectedFolder);
    });
    const auto visibleFolders =
        filter.empty()
            ? std::ranges::count_if(catalog.folders, [&](const auto& f) { return f.parent_path() == selectedFolder; })
            : 0;

    ImGui::BeginChild("##asset-browser", {0.0F, -24.0F}, false);
    const float treeWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.22F, 170.0F, 245.0F);
    ImGui::BeginChild("##asset-tree", {treeWidth, 0.0F}, true);
    ImGui::TextDisabled("PROJECT");
    ImGui::Separator();
    if (ImGui::Selectable("Assets", selectedFolder.empty(), 0, {0.0F, 25.0F})) {
        selectedFolder.clear();
        selected.clear();
        error.clear();
    }
    if (projectIsOpen && ImGui::BeginPopupContextItem("##assets-root-context")) {
        drawBackgroundMenu({});
        ImGui::EndPopup();
    }
    draw_folder_tree({}, catalog.folders, selectedFolder, selected, drawFolderMenu);
    if (projectIsOpen && ImGui::BeginPopupContextWindow("##asset-tree-context", ImGuiPopupFlags_MouseButtonRight |
                                                                                    ImGuiPopupFlags_NoOpenOverItems)) {
        drawBackgroundMenu({});
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    const bool inspectorVisible = showInspector && !selected.empty() && ImGui::GetContentRegionAvail().x > 560.0F;
    const float inspectorWidth =
        inspectorVisible ? std::clamp(ImGui::GetContentRegionAvail().x * 0.28F, 210.0F, 300.0F) : 0.0F;
    ImGui::BeginChild("##asset-content", {-inspectorWidth, 0.0F}, false);
    draw_breadcrumbs(selectedFolder, selected);
    if (!filter.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Search results");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##search"))
            filter.clear();
    }
    ImGui::Separator();

    if (gridView) {
        const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (tileSize + 24.0F)));
        if (ImGui::BeginTable("##asset-grid", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
            if (filter.empty())
                for (const auto& folder : catalog.folders) {
                    if (folder.parent_path() != selectedFolder)
                        continue;
                    ImGui::TableNextColumn();
                    ImGui::PushID(folder.generic_string().c_str());
                    const ImVec2 size{std::max(60.0F, ImGui::GetContentRegionAvail().x - 4.0F), tileSize + 39.0F};
                    ImGui::Selectable("##folder-card", false, 0, size);
                    const bool open = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                    const auto min = ImGui::GetItemRectMin();
                    draw_folder_icon({min.x + (size.x - tileSize * 0.66F) * 0.5F, min.y + 8.0F}, tileSize * 0.66F,
                                     false);
                    const auto name = clipped_label(folder.filename().string(), size.x - 12.0F);
                    ImGui::GetWindowDrawList()->AddText({min.x + 6.0F, min.y + tileSize + 15.0F},
                                                        ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
                    if (open) {
                        selectedFolder = folder;
                        selected.clear();
                    }
                    if (ImGui::BeginPopupContextItem("##folder-context")) {
                        drawFolderMenu(folder);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            for (const auto& asset : catalog.files) {
                if (!matches_filter(asset, filterLower) ||
                    (filter.empty() && asset.relative.parent_path() != selectedFolder))
                    continue;
                ImGui::TableNextColumn();
                ImGui::PushID(asset.relative.generic_string().c_str());
                const bool isSelected = selected == asset.relative;
                const ImVec2 size{std::max(60.0F, ImGui::GetContentRegionAvail().x - 4.0F), tileSize + 39.0F};
                if (ImGui::Selectable("##asset-card", isSelected, 0, size)) {
                    selected = asset.relative;
                    error.clear();
                }
                const bool isEnvironment = lower(asset.relative.extension().string()) == ".hdr" ||
                                           lower(asset.relative.extension().string()) == ".exr";
                if ((is_model(asset.relative) || asset.kind == AssetKind::Texture || is_shader_graph(asset.relative) ||
                     isEnvironment) &&
                    ImGui::BeginDragDropSource()) {
                    selected = asset.relative;
                    if (is_model(asset.relative))
                        Editor::AssetDragDrop::setModelPayload(asset.relative);
                    else if (is_shader_graph(asset.relative))
                        Editor::AssetDragDrop::setShaderGraphPayload(asset.relative);
                    else if (isEnvironment)
                        Editor::AssetDragDrop::setEnvironmentPayload(asset.relative);
                    else
                        Editor::AssetDragDrop::setTexturePayload(asset.relative);
                    ImGui::TextUnformatted(isEnvironment ? "Set scene environment"
                                                         : (is_shader_graph(asset.relative) ? "Assign Shader Graph"
                                                                                            : "Add model to scene"));
                    ImGui::TextDisabled("%s", asset.relative.filename().string().c_str());
                    ImGui::EndDragDropSource();
                }
                const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                const auto min = ImGui::GetItemRectMin();
                const float iconSize = tileSize * 0.62F;
                draw_file_icon({min.x + (size.x - iconSize) * 0.5F, min.y + 8.0F}, iconSize, asset.kind, isSelected);
                const auto name = clipped_label(asset.relative.stem().string(), size.x - 12.0F);
                const auto ext = lower(asset.relative.extension().string());
                ImGui::GetWindowDrawList()->AddText({min.x + 6.0F, min.y + tileSize + 8.0F},
                                                    ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
                ImGui::GetWindowDrawList()->AddText({min.x + 6.0F, min.y + tileSize + 24.0F},
                                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), ext.c_str());
                if (doubleClicked) {
                    if (is_model(asset.relative))
                        instantiate();
                    else if (is_shader_graph(asset.relative))
                        openShaderGraph(asset.relative);
                }
                if (ImGui::BeginPopupContextItem("##context")) {
                    ImGui::TextDisabled("%s", asset.relative.filename().string().c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Show details")) {
                        selected = asset.relative;
                        showInspector = true;
                    }
                    ImGui::BeginDisabled(!is_model(asset.relative));
                    if (ImGui::MenuItem("Add to scene")) {
                        selected = asset.relative;
                        instantiate();
                    }
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(!is_shader_graph(asset.relative));
                    if (ImGui::MenuItem("Open in Shader Graph"))
                        openShaderGraph(asset.relative);
                    ImGui::EndDisabled();
                    ImGui::Separator();
                    drawFileItems(asset.relative);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else if (ImGui::BeginTable("##asset-list", 3,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.62F);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.23F);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.15F);
        ImGui::TableHeadersRow();
        if (filter.empty())
            for (const auto& folder : catalog.folders) {
                if (folder.parent_path() != selectedFolder)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(folder.generic_string().c_str());
                if (ImGui::Selectable(("      " + folder.filename().string()).c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    selectedFolder = folder;
                    selected.clear();
                }
                {
                    const ImVec2 min = ImGui::GetItemRectMin();
                    const float iconSize = ImGui::GetFontSize() * 0.9F;
                    EditorUI::drawIcon(ImGui::GetWindowDrawList(), EditorUI::Icon::Folder,
                                       {min.x + iconSize * 0.5F + 2.0F, min.y + ImGui::GetTextLineHeight() * 0.5F},
                                       iconSize, ImGui::GetColorU32(EditorUI::colors().warning));
                }
                if (ImGui::BeginPopupContextItem("##folder-list-context")) {
                    drawFolderMenu(folder);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("Folder");
            }
        for (const auto& asset : catalog.files) {
            if (!matches_filter(asset, filterLower) ||
                (filter.empty() && asset.relative.parent_path() != selectedFolder))
                continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(asset.relative.generic_string().c_str());
            if (ImGui::Selectable(("      " + asset.relative.filename().string()).c_str(), selected == asset.relative,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                selected = asset.relative;
                error.clear();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (is_model(asset.relative))
                        instantiate();
                    else if (is_shader_graph(asset.relative))
                        openShaderGraph(asset.relative);
                }
            }
            {
                const ImVec2 min = ImGui::GetItemRectMin();
                const float iconSize = ImGui::GetFontSize() * 0.9F;
                EditorUI::drawIcon(ImGui::GetWindowDrawList(), kind_icon(asset.kind),
                                   {min.x + iconSize * 0.5F + 2.0F, min.y + ImGui::GetTextLineHeight() * 0.5F},
                                   iconSize, ImGui::GetColorU32(kind_color(asset.kind)));
            }
            const bool isEnvironment = lower(asset.relative.extension().string()) == ".hdr" ||
                                       lower(asset.relative.extension().string()) == ".exr";
            if ((is_model(asset.relative) || asset.kind == AssetKind::Texture || is_shader_graph(asset.relative) ||
                 isEnvironment) &&
                ImGui::BeginDragDropSource()) {
                selected = asset.relative;
                if (is_model(asset.relative))
                    Editor::AssetDragDrop::setModelPayload(asset.relative);
                else if (is_shader_graph(asset.relative))
                    Editor::AssetDragDrop::setShaderGraphPayload(asset.relative);
                else if (isEnvironment)
                    Editor::AssetDragDrop::setEnvironmentPayload(asset.relative);
                else
                    Editor::AssetDragDrop::setTexturePayload(asset.relative);
                ImGui::TextUnformatted(
                    isEnvironment ? "Set scene environment"
                                  : (is_shader_graph(asset.relative) ? "Assign Shader Graph" : "Add model to scene"));
                ImGui::TextDisabled("%s", asset.relative.filename().string().c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("##list-context")) {
                ImGui::TextDisabled("%s", asset.relative.filename().string().c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Show details")) {
                    selected = asset.relative;
                    showInspector = true;
                }
                ImGui::BeginDisabled(!is_model(asset.relative));
                if (ImGui::MenuItem("Add to scene")) {
                    selected = asset.relative;
                    instantiate();
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!is_shader_graph(asset.relative));
                if (ImGui::MenuItem("Open in Shader Graph"))
                    openShaderGraph(asset.relative);
                ImGui::EndDisabled();
                ImGui::Separator();
                drawFileItems(asset.relative);
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", kind_name(asset.kind));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", format_size(asset.size).c_str());
        }
        // The scrolling table is its own window: give its empty rows the
        // open folder's menu too.
        if (projectIsOpen &&
            ImGui::BeginPopupContextWindow("##asset-list-empty-context",
                                           ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            drawBackgroundMenu(selectedFolder);
            ImGui::EndPopup();
        }
        ImGui::EndTable();
    }

    if (visibleFiles == 0 && visibleFolders == 0) {
        ImGui::Dummy({0.0F, std::max(8.0F, ImGui::GetContentRegionAvail().y * 0.12F)});
        if (filter.empty())
            EditorUI::emptyState(EditorUI::Icon::Folder, "This folder is empty",
                                 "Right-click to create a folder, file or Shader Graph, or import assets.");
        else
            EditorUI::emptyState(EditorUI::Icon::Search, "No assets match your search",
                                 "Try a different name, file extension or asset type.");
    }
    // Empty space of the open folder; files and folders have their own menus.
    if (projectIsOpen &&
        ImGui::BeginPopupContextWindow("##asset-content-context",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        drawBackgroundMenu(selectedFolder);
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    if (openCreatePopup) {
        focusCreateName = true;
        ImGui::OpenPopup("##create-asset-entry");
    }
    if (EditorUI::beginDialog("##create-asset-entry", 400.0F)) {
        const bool isFolder = createKind == 1;
        EditorUI::dialogHeader(isFolder ? EditorUI::Icon::Folder : EditorUI::Icon::File, EditorUI::colors().accent,
                               isFolder ? "New folder" : "New file",
                               createParent.empty() ? "Created in Assets"
                                                    : ("Created in Assets/" + createParent.generic_string()).c_str());
        if (focusCreateName) {
            ImGui::SetKeyboardFocusHere();
            focusCreateName = false;
        }
        char nameBuffer[256]{};
        std::snprintf(nameBuffer, sizeof nameBuffer, "%s", createName.c_str());
        ImGui::SetNextItemWidth(-1.0F);
        const bool submitted =
            ImGui::InputText("##create-name", nameBuffer, sizeof nameBuffer, ImGuiInputTextFlags_EnterReturnsTrue);
        createName = nameBuffer;
        const std::filesystem::path name{createName};
        const bool validName = valid_asset_name(createName);
        const bool exists = validName && std::filesystem::exists(root / createParent / name);
        if (!validName)
            ImGui::TextColored(EditorUI::colors().warning, "Enter a valid name.");
        else if (exists)
            ImGui::TextColored(EditorUI::colors().warning, "'%s' already exists.", createName.c_str());
        EditorUI::dialogFooter(2);
        bool confirm = false;
        if (ImGui::Button("Cancel", {104.0F, 0.0F}) || EditorUI::dialogEscapePressed()) {
            createKind = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!validName || exists);
        if (EditorUI::primaryButton("Create", {104.0F, 0.0F}))
            confirm = true;
        ImGui::EndDisabled();
        if (submitted && validName && !exists)
            confirm = true;
        if (confirm) {
            const auto relative = createParent / name;
            try {
                if (createKind == 1) {
                    std::filesystem::create_directories(root / relative);
                    Editor::ConsolePanel::info("Created folder: " + relative.generic_string());
                } else {
                    std::filesystem::create_directories((root / relative).parent_path());
                    std::ofstream file{root / relative, std::ios::binary};
                    if (!file)
                        throw std::runtime_error("Could not create file '" + relative.generic_string() + "'");
                    Editor::ConsolePanel::info("Created file: " + relative.generic_string());
                }
                error.clear();
                refresh();
                if (createKind == 1) {
                    selectedFolder = relative;
                    selected.clear();
                } else {
                    // Show the new file where it was created.
                    selectedFolder = createParent;
                    selected = relative;
                }
            } catch (const std::exception& exception) {
                error = exception.what();
                Editor::ConsolePanel::error(error);
            }
            createKind = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (openRenamePopup) {
        focusRenameName = true;
        ImGui::OpenPopup("##rename-asset");
    }
    if (EditorUI::beginDialog("##rename-asset", 400.0F)) {
        const bool renamingFolder = std::filesystem::is_directory(root / renameSource);
        const std::string title = "Rename '" + renameSource.filename().string() + "'";
        EditorUI::dialogHeader(
            renamingFolder ? EditorUI::Icon::Folder : EditorUI::Icon::File, EditorUI::colors().accent, title.c_str(),
            renameSource.parent_path().empty() ? "In Assets"
                                               : ("In Assets/" + renameSource.parent_path().generic_string()).c_str());
        if (focusRenameName) {
            ImGui::SetKeyboardFocusHere();
            focusRenameName = false;
        }
        char nameBuffer[256]{};
        std::snprintf(nameBuffer, sizeof nameBuffer, "%s", renameName.c_str());
        ImGui::SetNextItemWidth(-1.0F);
        const bool submitted =
            ImGui::InputText("##rename-name", nameBuffer, sizeof nameBuffer, ImGuiInputTextFlags_EnterReturnsTrue);
        renameName = nameBuffer;
        const bool unchanged = renameName == renameSource.filename().string();
        const bool validName = valid_asset_name(renameName);
        const bool exists =
            validName && !unchanged && std::filesystem::exists(root / renameSource.parent_path() / renameName);
        if (!validName)
            ImGui::TextColored(EditorUI::colors().warning, "Enter a valid name.");
        else if (exists)
            ImGui::TextColored(EditorUI::colors().warning, "'%s' already exists.", renameName.c_str());
        else if (!renamingFolder && std::filesystem::path{renameName}.extension() != renameSource.extension())
            ImGui::TextColored(EditorUI::colors().warning, "The file extension changes.");
        EditorUI::dialogFooter(2);
        bool confirm = false;
        if (ImGui::Button("Cancel", {104.0F, 0.0F}) || EditorUI::dialogEscapePressed())
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::BeginDisabled(!validName || exists || unchanged);
        if (EditorUI::primaryButton("Rename", {104.0F, 0.0F}))
            confirm = true;
        ImGui::EndDisabled();
        if (submitted && validName && !exists && !unchanged)
            confirm = true;
        if (confirm) {
            std::filesystem::path renamed;
            const auto oldPath = (root / renameSource).lexically_normal();
            if (rename_asset(scene, root, renameSource, renameName, renamed, error)) {
                Editor::ConsolePanel::info("Renamed " + renameSource.generic_string() + " to " +
                                           renamed.generic_string());
                // Keep the open scene's path, also when it sits in a renamed folder.
                const auto openScene = EditorSceneSession::scenePath().lexically_normal();
                const auto inside = openScene.lexically_relative(oldPath);
                if (openScene == oldPath)
                    EditorSceneSession::setScenePath((root / renamed).lexically_normal());
                else if (renamingFolder && !inside.empty() && *inside.begin() != "..")
                    EditorSceneSession::setScenePath((root / renamed / inside).lexically_normal());
                error.clear();
                refresh();
                if (renamingFolder) {
                    if (selectedFolder == renameSource)
                        selectedFolder = renamed;
                } else {
                    selected = renamed;
                }
            } else {
                Editor::ConsolePanel::error(error);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (openDeletePopup)
        ImGui::OpenPopup("##confirm-delete-asset");
    if (EditorUI::beginDialog("##confirm-delete-asset", 400.0F)) {
        const bool deletingFolder = std::filesystem::is_directory(root / deleteCandidate);
        const std::string deleteTitle = "Delete '" + deleteCandidate.filename().string() + "'?";
        EditorUI::dialogHeader(EditorUI::Icon::Warning, EditorUI::colors().warning, deleteTitle.c_str(),
                               deletingFolder ? "This permanently removes the folder and everything in it."
                                              : "This removes the file from the project.");
        EditorUI::dialogFooter(2);
        if (ImGui::Button("Cancel", {104.0F, 0.0F}) || EditorUI::dialogEscapePressed()) {
            deleteCandidate.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorUI::dangerButton("Delete", {104.0F, 0.0F})) {
            if (deletingFolder) {
                std::vector<std::filesystem::path> deletedScenes;
                if (delete_asset_folder(scene, root, deleteCandidate, deletedScenes, error)) {
                    Editor::ConsolePanel::info("Deleted folder: " + deleteCandidate.generic_string());
                    for (const auto& deletedScene : deletedScenes)
                        sceneDeleted(deletedScene);
                    refresh();
                } else {
                    Editor::ConsolePanel::error(error);
                }
            } else if (delete_asset_file(scene, root, deleteCandidate, error)) {
                Editor::ConsolePanel::info("Deleted asset: " + deleteCandidate.generic_string());
                if (deleteCandidate.extension() == ".scene")
                    sceneDeleted((root / deleteCandidate).lexically_normal());
                refresh();
            } else {
                Editor::ConsolePanel::error(error);
            }
            deleteCandidate.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (inspectorVisible) {
        ImGui::SameLine();
        ImGui::BeginChild("##asset-inspector", {0.0F, 0.0F}, true);
        const auto it =
            std::ranges::find_if(catalog.files, [&](const AssetEntry& a) { return a.relative == selected; });
        if (it != catalog.files.end()) {
            const float preview = std::min(76.0F, ImGui::GetContentRegionAvail().x * 0.38F);
            draw_file_icon(ImGui::GetCursorScreenPos(), preview, it->kind, true);
            ImGui::Dummy({preview, preview + 5.0F});
            ImGui::TextWrapped("%s", selected.stem().string().c_str());
            EditorUI::badge(kind_name(it->kind), EditorUI::withAlpha(kind_color(it->kind), 0.16F),
                            kind_color(it->kind));
            ImGui::SeparatorText("Details");
            ImGui::TextDisabled("Path");
            ImGui::TextWrapped("Assets/%s", selected.generic_string().c_str());
            ImGui::TextDisabled("Size");
            ImGui::SameLine(80.0F);
            ImGui::TextUnformatted(format_size(it->size).c_str());
            if (is_model(selected)) {
                const auto dependencies = gltf_dependencies(root / selected);
                if (ImGui::TreeNodeEx("Dependencies", ImGuiTreeNodeFlags_DefaultOpen, "Dependencies (%zu)",
                                      dependencies.size())) {
                    if (dependencies.empty())
                        ImGui::TextDisabled("Embedded or none");
                    for (const auto& dependency : dependencies) {
                        std::error_code dependencyError;
                        const bool exists = std::filesystem::is_regular_file(dependency, dependencyError);
                        EditorUI::iconLabel(exists ? EditorUI::Icon::Info : EditorUI::Icon::Error,
                                            exists ? EditorUI::colors().success : EditorUI::colors().error);
                        ImGui::SameLine(0.0F, 6.0F);
                        ImGui::TextUnformatted(dependency.filename().string().c_str());
                        if (!exists && ImGui::IsItemHovered())
                            ImGui::SetTooltip("Missing dependency");
                    }
                    ImGui::TreePop();
                }
                ImGui::Spacing();
                if (EditorUI::primaryButton("Add to scene", {-1.0F, 0.0F}))
                    instantiate();
            } else {
                ImGui::Spacing();
                ImGui::TextDisabled("Preview/import settings are not available for this asset type yet.");
            }
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorUI::withAlpha(EditorUI::colors().error, 0.35F));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorUI::withAlpha(EditorUI::colors().error, 0.55F));
            if (ImGui::Button("Delete asset...", {-1.0F, 0.0F}))
                requestDelete(selected);
            ImGui::PopStyleColor(2);
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::TextDisabled("%zu item%s", visibleFiles + visibleFolders, visibleFiles + visibleFolders == 1 ? "" : "s");
    if (!selected.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  ·  %s", selected.filename().string().c_str());
    }
    if (!error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(EditorUI::colors().error, "  ·  %s", error.c_str());
    }
    if (disabled) {
        ImGui::SameLine();
        ImGui::TextColored(EditorUI::colors().warning, "  ·  Play Mode: additions are temporary");
    }
    if (!projectIsOpen) {
        ImGui::SameLine();
        ImGui::TextColored(EditorUI::colors().warning, "  ·  Open or create a project before importing assets");
    }
    ImGui::End();
    return created;
}

// NOLINTEND(readability-magic-numbers)
// NOLINTEND(readability-identifier-length)
