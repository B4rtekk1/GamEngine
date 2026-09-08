#include "Editor/Panels/ShaderGraphPanel.h"

#include "Engine/Renderer/ShaderGraph/ShaderNodeFactory.h"
#include "Engine/Renderer/ShaderGraph/ShaderNodeRegistry.h"
#include "Engine/Renderer/ShaderGraph/ShaderGraphSerializer.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace Editor {
    namespace {
        const char* nodeName(const Engine::ShaderNodeType type) {
            if (const auto* definition = Engine::ShaderNodeRegistry::find(type)) return definition->displayName.data();
            return "Unknown";
        }

        ImU32 pinColor(const Engine::ShaderValueType type) {
            using Type = Engine::ShaderValueType;
            switch (type) {
                case Type::Float: return IM_COL32(180, 180, 180, 255);
                case Type::Float2: return IM_COL32(90, 190, 120, 255);
                case Type::Float3: return IM_COL32(225, 185, 70, 255);
                case Type::Float4: return IM_COL32(175, 100, 220, 255);
                case Type::Bool: return IM_COL32(210, 80, 80, 255);
                case Type::Texture2D: return IM_COL32(80, 140, 230, 255);
            }
            return IM_COL32_WHITE;
        }

        bool canConnect(const Engine::ShaderValueType from, const Engine::ShaderValueType to) {
            return from == to || (from == Engine::ShaderValueType::Float &&
                                  (to == Engine::ShaderValueType::Float2 ||
                                   to == Engine::ShaderValueType::Float3 ||
                                   to == Engine::ShaderValueType::Float4));
        }
    } // namespace

    ShaderGraphPanel::ShaderGraphPanel() : editorContext_(ImNodes::EditorContextCreate()) {}

    ShaderGraphPanel::~ShaderGraphPanel() {
        if (editorContext_ != nullptr) ImNodes::EditorContextFree(editorContext_);
    }

    void ShaderGraphPanel::open(Engine::ShaderGraphAsset& graph, std::filesystem::path assetPath) {
        graph_ = &graph;
        assetPath_ = std::move(assetPath);
        nextNodeId_ = 1;
        nextPinId_ = 1;
        nextLinkId_ = 1;
        for (const auto& node : graph.nodes) {
            nextNodeId_ = std::max(nextNodeId_, node.id + 1);
            for (const auto& pin : node.inputs) nextPinId_ = std::max(nextPinId_, pin.id + 1);
            for (const auto& pin : node.outputs) nextPinId_ = std::max(nextPinId_, pin.id + 1);
        }
        for (const auto& link : graph.links) nextLinkId_ = std::max(nextLinkId_, link.id + 1);
        initializedPositions_.clear();
        ImNodes::EditorContextSet(editorContext_);
        ImNodes::EditorContextResetPanning({0.0F, 0.0F});
    }

    void ShaderGraphPanel::draw(bool& isOpen) {
        if (graph_ == nullptr) return;
        if (!ImGui::Begin("Shader Graph", &isOpen)) {
            ImGui::End();
            return;
        }
        ImGui::TextUnformatted(graph_->name.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(assetPath_.empty());
        if (ImGui::Button("Save") ||
            (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))) {
            try {
                Engine::ShaderGraphSerializer::save(*graph_, assetPath_);
                codeDirty_ = false;
                layoutDirty_ = false;
            } catch (const std::exception& error) {
                ImGui::TextColored({1.0F, 0.35F, 0.35F, 1.0F}, "%s", error.what());
            }
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        constexpr float blackboardWidth = 240.0F;
        ImGui::BeginChild("##shader-blackboard", {blackboardWidth, 0.0F}, ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("Properties");
        ImGui::Separator();
        if (graph_->properties.empty()) ImGui::TextDisabled("No properties");
        for (const auto& property : graph_->properties) ImGui::TextUnformatted(property.name.c_str());
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##shader-canvas", {0.0F, 0.0F});
        drawCanvas();
        ImGui::EndChild();
        ImGui::End();
    }

    void ShaderGraphPanel::drawCanvas() {
        ImNodes::EditorContextSet(editorContext_);
        ImNodes::BeginNodeEditor();
        drawCreateNodePopup();
        for (auto& node : graph_->nodes) drawNode(node);
        for (const auto& link : graph_->links)
            ImNodes::Link(static_cast<int>(link.id), static_cast<int>(link.fromPin), static_cast<int>(link.toPin));
        ImNodes::MiniMap(0.18F, ImNodesMiniMapLocation_BottomRight);
        ImNodes::EndNodeEditor();
        handleLinkCreation();
        handleLinkDeletion();
        handleSelectionDeletion();
        for (auto& node : graph_->nodes) {
            const ImVec2 position = ImNodes::GetNodeGridSpacePos(static_cast<int>(node.id));
            const Engine::Vec2 previous = node.editorPosition;
            node.editorPosition = {position.x, position.y};
            layoutDirty_ = layoutDirty_ || previous.x() != position.x || previous.y() != position.y;
        }
    }

    void ShaderGraphPanel::drawNode(Engine::ShaderNode& node) {
        if (!initializedPositions_.contains(node.id)) {
            ImNodes::SetNodeGridSpacePos(static_cast<int>(node.id),
                                         {node.editorPosition.x(), node.editorPosition.y()});
            initializedPositions_.insert(node.id);
        }
        ImNodes::BeginNode(static_cast<int>(node.id));
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(nodeName(node.type));
        ImNodes::EndNodeTitleBar();
        for (auto& pin : node.inputs) drawInputPin(pin);
        for (auto& pin : node.outputs) {
            ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkDetachWithDragClick);
            drawOutputPin(pin);
            ImNodes::PopAttributeFlag();
        }
        if (node.type == Engine::ShaderNodeType::Float) {
            if (auto* value = std::get_if<float>(&node.value)) {
                ImGui::SetNextItemWidth(100.0F);
                if (ImGui::DragFloat("##value", value, 0.01F)) codeDirty_ = true;
            }
        }
        ImNodes::EndNode();
    }

    void ShaderGraphPanel::drawInputPin(Engine::ShaderPin& pin) {
        ImNodes::PushColorStyle(ImNodesCol_Pin, pinColor(pin.type));
        ImNodes::BeginInputAttribute(static_cast<int>(pin.id), ImNodesPinShape_CircleFilled);
        ImGui::TextUnformatted(pin.name.c_str());
        ImNodes::EndInputAttribute();
        ImNodes::PopColorStyle();
    }

    void ShaderGraphPanel::drawOutputPin(Engine::ShaderPin& pin) {
        ImNodes::PushColorStyle(ImNodesCol_Pin, pinColor(pin.type));
        ImNodes::BeginOutputAttribute(static_cast<int>(pin.id), ImNodesPinShape_CircleFilled);
        ImGui::TextUnformatted(pin.name.c_str());
        ImNodes::EndOutputAttribute();
        ImNodes::PopColorStyle();
    }

    void ShaderGraphPanel::drawCreateNodePopup() {
        if (ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##shader-create-node");
        if (!ImGui::BeginPopup("##shader-create-node")) return;
        const ImVec2 position = ImGui::GetMousePosOnOpeningCurrentPopup();
        const auto drawNodes = [&](const std::string_view category, const std::string_view subcategory) {
            for (const auto& definition : Engine::ShaderNodeRegistry::definitions()) {
                if (definition.category == category && definition.subcategory == subcategory &&
                    definition.compileKind != Engine::ShaderNodeCompileKind::Unsupported &&
                    definition.type != Engine::ShaderNodeType::SurfaceOutput && ImGui::MenuItem(definition.displayName.data()))
                    createNode(definition.type, position);
            }
        };
        for (const std::string_view category : {"Input", "Math", "Utility", "Texture"}) {
            if (!ImGui::BeginMenu(category.data())) continue;
            if (category == "Math") {
                for (const std::string_view subcategory : {"Basic", "Trigonometry", "Vector"}) {
                    if (ImGui::BeginMenu(subcategory.data())) {
                        drawNodes(category, subcategory);
                        ImGui::EndMenu();
                    }
                }
            } else {
                drawNodes(category, {});
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    void ShaderGraphPanel::createNode(const Engine::ShaderNodeType type, const ImVec2 screenPosition) {
        Engine::ShaderNode node = Engine::ShaderNodeFactory::create(type, nextNodeId_++, nextPinId_);
        const Engine::ShaderNodeId id = node.id;
        graph_->nodes.push_back(std::move(node));
        ImNodes::SetNodeScreenSpacePos(static_cast<int>(id), screenPosition);
        initializedPositions_.insert(id);
        const ImVec2 gridPosition = ImNodes::GetNodeGridSpacePos(static_cast<int>(id));
        graph_->nodes.back().editorPosition = {gridPosition.x, gridPosition.y};
        codeDirty_ = true;
        layoutDirty_ = true;
    }

    void ShaderGraphPanel::handleLinkCreation() {
        int first = 0;
        int second = 0;
        if (ImNodes::IsLinkCreated(&first, &second))
            static_cast<void>(tryCreateLink(static_cast<Engine::ShaderPinId>(first),
                                            static_cast<Engine::ShaderPinId>(second)));
    }

    void ShaderGraphPanel::handleLinkDeletion() {
        int id = 0;
        if (ImNodes::IsLinkDestroyed(&id)) deleteLink(static_cast<Engine::ShaderLinkId>(id));
    }

    void ShaderGraphPanel::handleSelectionDeletion() {
        if (!ImNodes::IsEditorHovered() || !ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::GetIO().WantTextInput)
            return;
        std::vector<int> links(static_cast<std::size_t>(ImNodes::NumSelectedLinks()));
        if (!links.empty()) {
            ImNodes::GetSelectedLinks(links.data());
            for (const int id : links) deleteLink(static_cast<Engine::ShaderLinkId>(id));
        }
        std::vector<int> nodes(static_cast<std::size_t>(ImNodes::NumSelectedNodes()));
        if (!nodes.empty()) {
            ImNodes::GetSelectedNodes(nodes.data());
            for (const int id : nodes) deleteNode(static_cast<Engine::ShaderNodeId>(id));
        }
    }

    bool ShaderGraphPanel::tryCreateLink(const Engine::ShaderPinId first, const Engine::ShaderPinId second) {
        const bool firstOutput = isOutputPin(first);
        const Engine::ShaderPinId output = firstOutput ? first : second;
        const Engine::ShaderPinId input = firstOutput ? second : first;
        if (!(firstOutput ? isInputPin(second) : isOutputPin(second) && isInputPin(first))) return false;
        const Engine::ShaderPin* outputPin = findPin(output);
        const Engine::ShaderPin* inputPin = findPin(input);
        if (outputPin == nullptr || inputPin == nullptr || !canConnect(outputPin->type, inputPin->type)) return false;
        std::erase_if(graph_->links, [input](const Engine::ShaderLink& link) { return link.toPin == input; });
        graph_->links.push_back({.id = nextLinkId_++, .fromPin = output, .toPin = input});
        codeDirty_ = true;
        return true;
    }

    Engine::ShaderPin* ShaderGraphPanel::findPin(const Engine::ShaderPinId id) {
        for (auto& node : graph_->nodes) {
            const auto input = std::ranges::find(node.inputs, id, &Engine::ShaderPin::id);
            if (input != node.inputs.end()) return &*input;
            const auto output = std::ranges::find(node.outputs, id, &Engine::ShaderPin::id);
            if (output != node.outputs.end()) return &*output;
        }
        return nullptr;
    }

    bool ShaderGraphPanel::isInputPin(const Engine::ShaderPinId id) const {
        return std::ranges::any_of(graph_->nodes, [id](const Engine::ShaderNode& node) {
            return std::ranges::any_of(node.inputs, [id](const Engine::ShaderPin& pin) { return pin.id == id; });
        });
    }

    bool ShaderGraphPanel::isOutputPin(const Engine::ShaderPinId id) const {
        return std::ranges::any_of(graph_->nodes, [id](const Engine::ShaderNode& node) {
            return std::ranges::any_of(node.outputs, [id](const Engine::ShaderPin& pin) { return pin.id == id; });
        });
    }

    void ShaderGraphPanel::deleteNode(const Engine::ShaderNodeId id) {
        const auto node = std::ranges::find(graph_->nodes, id, &Engine::ShaderNode::id);
        if (node == graph_->nodes.end() || node->type == Engine::ShaderNodeType::SurfaceOutput) return;
        std::unordered_set<Engine::ShaderPinId> pins;
        for (const auto& pin : node->inputs) pins.insert(pin.id);
        for (const auto& pin : node->outputs) pins.insert(pin.id);
        std::erase_if(graph_->links, [&pins](const Engine::ShaderLink& link) {
            return pins.contains(link.fromPin) || pins.contains(link.toPin);
        });
        graph_->nodes.erase(node);
        initializedPositions_.erase(id);
        codeDirty_ = true;
    }

    void ShaderGraphPanel::deleteLink(const Engine::ShaderLinkId id) {
        const auto previousSize = graph_->links.size();
        std::erase_if(graph_->links, [id](const Engine::ShaderLink& link) { return link.id == id; });
        codeDirty_ = codeDirty_ || graph_->links.size() != previousSize;
    }
} // namespace Editor
