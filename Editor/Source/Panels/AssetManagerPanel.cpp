#include "Editor/Panels/AssetManagerPanel.h"

#include "Editor/AssetImporter.h"
#include "Editor/Panels/AssetDragDrop.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Elements/NumericControl.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <regex>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

// NOLINTBEGIN(readability-magic-numbers)
// NOLINTBEGIN(readability-identifier-length)

namespace {

enum class AssetKind { Model, Texture, Shader, Scene, Audio, Other };

struct AssetEntry final {
    std::filesystem::path relative;
    std::uintmax_t size{};
    AssetKind kind{AssetKind::Other};
};

struct AssetCatalog final {
    std::vector<AssetEntry> files;
    std::vector<std::filesystem::path> folders;
};

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

#ifdef _WIN32
std::optional<std::filesystem::path> chooseImportFile(const bool archiveOnly) {
    std::array<wchar_t, 32768> path{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = archiveOnly ? L"ZIP archive (*.zip)\0*.zip\0\0"
                                    : L"All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path{path.data()};
}

std::optional<std::filesystem::path> chooseImportFolder() {
    BROWSEINFOW dialog{};
    dialog.lpszTitle = L"Choose asset folder to import";
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&dialog);
    if (item == nullptr) return std::nullopt;
    std::array<wchar_t, MAX_PATH> path{};
    const bool resolved = SHGetPathFromIDListW(item, path.data()) != FALSE;
    CoTaskMemFree(item);
    return resolved ? std::optional<std::filesystem::path>{std::filesystem::path{path.data()}} : std::nullopt;
}
#endif

bool is_model(const std::filesystem::path& path) {
    const auto ext = lower(path.extension().string());
    return ext == ".gltf" || ext == ".glb";
}

AssetKind asset_kind(const std::filesystem::path& path) {
    const auto ext = lower(path.extension().string());
    if (ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".fbx")
        return AssetKind::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr")
        return AssetKind::Texture;
    if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".glsl" || ext == ".slang" || ext == ".spv")
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
            result.files.push_back({relative, it->file_size(error), asset_kind(relative)});
        error.clear();
    }
    std::ranges::sort(result.files, [](const AssetEntry& a, const AssetEntry& b) {
        return lower(a.relative.generic_string()) < lower(b.relative.generic_string());
    });
    std::ranges::sort(result.folders, [](const auto& a, const auto& b) {
        return lower(a.generic_string()) < lower(b.generic_string());
    });
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

void draw_folder_icon(const ImVec2 min, const float size, const bool selected) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 back =
        ImGui::GetColorU32(selected ? ImVec4{0.25F, 0.72F, 0.88F, 1.0F} : ImVec4{0.78F, 0.61F, 0.29F, 1.0F});
    const ImU32 front =
        ImGui::GetColorU32(selected ? ImVec4{0.38F, 0.82F, 0.95F, 1.0F} : ImVec4{0.94F, 0.76F, 0.38F, 1.0F});
    const float y = min.y + size * 0.16F;
    draw->AddRectFilled({min.x, y + size * 0.14F}, {min.x + size, y + size * 0.75F}, back, size * 0.08F);
    draw->AddRectFilled({min.x + size * 0.06F, y}, {min.x + size * 0.48F, y + size * 0.29F}, back, size * 0.06F);
    draw->AddRectFilled({min.x, y + size * 0.27F}, {min.x + size, y + size * 0.82F}, front, size * 0.08F);
}

void draw_file_icon(const ImVec2 min, const float size, const AssetKind kind, const bool selected) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec4 accent = selected ? ImVec4{0.39F, 0.86F, 0.96F, 1.0F} : kind_color(kind);
    const ImU32 color = ImGui::GetColorU32(accent);
    const ImU32 fill = ImGui::GetColorU32(ImVec4{accent.x * 0.24F, accent.y * 0.24F, accent.z * 0.24F, 1.0F});
    const float fold = size * 0.25F;
    draw->AddRectFilled(min, {min.x + size, min.y + size}, fill, size * 0.08F);
    draw->AddRect(min, {min.x + size, min.y + size}, color, size * 0.08F, 0, 1.5F);
    draw->AddLine({min.x + size - fold, min.y}, {min.x + size - fold, min.y + fold}, color, 1.3F);
    draw->AddLine({min.x + size - fold, min.y + fold}, {min.x + size, min.y + fold}, color, 1.3F);
    const char* mark = kind == AssetKind::Model     ? "3D"
                       : kind == AssetKind::Texture ? "IMG"
                       : kind == AssetKind::Shader  ? "<>"
                       : kind == AssetKind::Scene   ? "SCN"
                       : kind == AssetKind::Audio   ? "SND"
                                                    : "FILE";
    const auto textSize = ImGui::CalcTextSize(mark);
    draw->AddText({min.x + (size - textSize.x) * 0.5F, min.y + size * 0.54F - textSize.y * 0.5F}, color, mark);
}

bool matches_filter(const AssetEntry& asset, const std::string& filter) {
    return filter.empty() || lower(asset.relative.generic_string()).find(filter) != std::string::npos ||
           lower(kind_name(asset.kind)).find(filter) != std::string::npos;
}

void draw_folder_tree(const std::filesystem::path& parent, const std::vector<std::filesystem::path>& folders,
                      std::filesystem::path& selectedFolder, std::filesystem::path& selectedAsset) {
    for (const auto& folder : folders) {
        if (folder.parent_path() != parent)
            continue;
        ImGui::PushID(folder.generic_string().c_str());
        auto flags =
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (selectedFolder == folder)
            flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(folder.filename().string().c_str(), flags);
        if (ImGui::IsItemClicked()) {
            selectedFolder = folder;
            selectedAsset.clear();
        }
        if (open) {
            draw_folder_tree(folder, folders, selectedFolder, selectedAsset);
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
        ImGui::SameLine(0.0F, 4.0F);
        ImGui::TextDisabled(">");
        ImGui::SameLine(0.0F, 4.0F);
        ImGui::PushID(accumulated.generic_string().c_str());
        if (ImGui::SmallButton(part.string().c_str())) {
            folder = accumulated;
            selectedAsset.clear();
        }
        ImGui::PopID();
    }
}

} // namespace

Engine::Entity AssetManagerPanel::draw(Engine::ScenePreset& scene, Engine::Assets::Content& content,
                                       const bool disabled, bool& isOpen, const bool projectIsOpen) {
    static std::filesystem::path selected;
    static std::filesystem::path selectedFolder;
    static std::filesystem::path scannedRoot;
    static std::string filter;
    static std::string error;
    static AssetCatalog catalog;
    static float tileSize = 92.0F;
    static bool gridView = true;
    static bool showInspector = true;
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
    const auto instantiate = [&] {
        if (selected.empty() || !is_model(selected))
            return;
        try {
            created = Editor::AssetDragDrop::instantiateModel(scene, content, selected);
            error.clear();
        } catch (const std::exception& exception) {
            error = exception.what();
        }
    };

    ImGui::Begin("Asset Manager", &isOpen);
    ImGui::BeginDisabled(selectedFolder.empty());
    if (ImGui::Button("<##asset-up")) {
        selectedFolder = selectedFolder.parent_path();
        selected.clear();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Go to parent folder");
    ImGui::SameLine();
    if (ImGui::Button("R##asset-refresh"))
        refresh();
    if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Refresh assets");
    ImGui::SameLine();
    ImGui::BeginDisabled(!projectIsOpen);
    if (ImGui::Button("Import...##asset-import"))
        ImGui::OpenPopup("##asset-import-menu");
    if (ImGui::BeginPopup("##asset-import-menu")) {
#ifdef _WIN32
        const auto importSelected = [&](const std::optional<std::filesystem::path>& source) {
            if (!source) return;
            if (Editor::AssetImporter::import(*source, root / selectedFolder, error)) {
                refresh();
                Editor::ConsolePanel::info("Imported asset: " + source->filename().string());
            } else {
                Editor::ConsolePanel::error("Could not import asset: " + error);
            }
        };
        if (ImGui::MenuItem("File...")) importSelected(chooseImportFile(false));
        if (ImGui::MenuItem("Folder...")) importSelected(chooseImportFolder());
        if (ImGui::MenuItem("ZIP archive...")) importSelected(chooseImportFile(true));
#else
        ImGui::TextDisabled("Asset import dialog is available in the Windows editor.");
#endif
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    char filterBuffer[256]{};
    std::snprintf(filterBuffer, sizeof(filterBuffer), "%s", filter.c_str());
    ImGui::SetNextItemWidth(std::max(140.0F, ImGui::GetContentRegionAvail().x - 232.0F));
    if (ImGui::InputTextWithHint("##asset-filter", "Search all assets...", filterBuffer, sizeof(filterBuffer)))
        filter = filterBuffer;
    ImGui::SameLine();
    if (ImGui::SmallButton(gridView ? "Grid" : "List"))
        gridView = !gridView;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(82.0F);
    Editor::Controls::sliderFloat("##asset-size", &tileSize, 72.0F, 136.0F, "");
    ImGui::SameLine();
    if (ImGui::SmallButton(showInspector ? "Info on" : "Info off"))
        showInspector = !showInspector;

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
    draw_folder_tree({}, catalog.folders, selectedFolder, selected);
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
                if ((is_model(asset.relative) || asset.kind == AssetKind::Texture) && ImGui::BeginDragDropSource()) {
                    selected = asset.relative;
                    if (is_model(asset.relative)) Editor::AssetDragDrop::setModelPayload(asset.relative);
                    else Editor::AssetDragDrop::setTexturePayload(asset.relative);
                    ImGui::TextUnformatted("Add model to scene");
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
                if (doubleClicked && is_model(asset.relative))
                    instantiate();
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
                if (ImGui::Selectable(("[Folder]  " + folder.filename().string()).c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    selectedFolder = folder;
                    selected.clear();
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
            if (ImGui::Selectable(asset.relative.filename().string().c_str(), selected == asset.relative,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                selected = asset.relative;
                error.clear();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && is_model(asset.relative))
                    instantiate();
            }
            if ((is_model(asset.relative) || asset.kind == AssetKind::Texture) && ImGui::BeginDragDropSource()) {
                selected = asset.relative;
                if (is_model(asset.relative)) Editor::AssetDragDrop::setModelPayload(asset.relative);
                else Editor::AssetDragDrop::setTexturePayload(asset.relative);
                ImGui::TextUnformatted("Add model to scene");
                ImGui::TextDisabled("%s", asset.relative.filename().string().c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", kind_name(asset.kind));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", format_size(asset.size).c_str());
        }
        ImGui::EndTable();
    }

    if (visibleFiles == 0 && visibleFolders == 0) {
        const auto available = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(12.0F, available.y * 0.24F));
        const char* message = filter.empty() ? "This folder is empty" : "No assets match your search";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0F, (available.x - ImGui::CalcTextSize(message).x) * 0.5F));
        ImGui::TextDisabled("%s", message);
    }
    ImGui::EndChild();

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
            ImGui::TextDisabled("%s", kind_name(it->kind));
            ImGui::Separator();
            ImGui::TextDisabled("PATH");
            ImGui::TextWrapped("Assets/%s", selected.generic_string().c_str());
            ImGui::TextDisabled("SIZE");
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
                        ImGui::TextColored(exists ? ImVec4{0.35F, 0.82F, 0.59F, 1.0F}
                                                  : ImVec4{0.95F, 0.40F, 0.35F, 1.0F},
                                           "%s %s", exists ? "OK" : "!", dependency.filename().string().c_str());
                    }
                    ImGui::TreePop();
                }
                if (ImGui::Button("Add to scene", {-1.0F, 0.0F}))
                    instantiate();
            } else {
                ImGui::Spacing();
                ImGui::TextDisabled("Preview/import settings are not available for this asset type yet.");
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::TextDisabled("%zu item%s", visibleFiles + visibleFolders, visibleFiles + visibleFolders == 1 ? "" : "s");
    if (!selected.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  |  %s", selected.filename().string().c_str());
    }
    if (!error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored({0.95F, 0.40F, 0.35F, 1.0F}, "  |  %s", error.c_str());
    }
    if (disabled) {
        ImGui::SameLine();
        ImGui::TextColored({0.95F, 0.68F, 0.28F, 1.0F}, "  |  Play Mode: additions are temporary");
    }
    if (!projectIsOpen) {
        ImGui::SameLine();
        ImGui::TextColored({0.95F, 0.68F, 0.28F, 1.0F},
                           "  |  Open or create a project before importing assets");
    }
    ImGui::End();
    return created;
}

// NOLINTEND(readability-magic-numbers)
// NOLINTEND(readability-identifier-length)
