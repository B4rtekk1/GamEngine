#pragma once

#include "Engine/Renderer/ShaderGraph/ShaderGraph.h"

#include <imnodes.h>

#include <filesystem>
#include <unordered_set>

namespace Editor {
    /** Immediate-mode editor view over an Engine-owned shader graph asset. */
    class ShaderGraphPanel final {
    public:
        ShaderGraphPanel();
        ~ShaderGraphPanel();
        ShaderGraphPanel(const ShaderGraphPanel&) = delete;
        ShaderGraphPanel& operator=(const ShaderGraphPanel&) = delete;

        void open(Engine::ShaderGraphAsset& graph, std::filesystem::path assetPath = {});
        void draw(bool& isOpen);

    private:
        Engine::ShaderGraphAsset* graph_{};
        ImNodesEditorContext* editorContext_{};
        Engine::ShaderNodeId nextNodeId_{1};
        Engine::ShaderPinId nextPinId_{1};
        Engine::ShaderLinkId nextLinkId_{1};
        std::unordered_set<Engine::ShaderNodeId> initializedPositions_;
        bool codeDirty_{};
        bool layoutDirty_{};
        std::filesystem::path assetPath_;

        void drawCanvas();
        void drawNode(Engine::ShaderNode& node);
        void drawInputPin(Engine::ShaderPin& pin);
        void drawOutputPin(Engine::ShaderPin& pin);
        void drawCreateNodePopup();
        void createNode(Engine::ShaderNodeType type, ImVec2 screenPosition);
        void handleLinkCreation();
        void handleLinkDeletion();
        void handleSelectionDeletion();
        [[nodiscard]] bool tryCreateLink(Engine::ShaderPinId first, Engine::ShaderPinId second);
        [[nodiscard]] Engine::ShaderPin* findPin(Engine::ShaderPinId id);
        [[nodiscard]] bool isInputPin(Engine::ShaderPinId id) const;
        [[nodiscard]] bool isOutputPin(Engine::ShaderPinId id) const;
        void deleteNode(Engine::ShaderNodeId id);
        void deleteLink(Engine::ShaderLinkId id);
    };
} // namespace Editor
