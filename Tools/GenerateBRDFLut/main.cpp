#include "Engine/Renderer/Lighting/EnvironmentBaker.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
constexpr std::uint32_t BrdfVersion = 3;
constexpr std::uint32_t LutSize = 256;
constexpr std::uint32_t SampleCount = 1024;

struct Header {
    std::array<char, 4> magic{'B', 'R', 'D', 'F'};
    std::uint32_t version{BrdfVersion};
    std::uint32_t width{LutSize};
    std::uint32_t height{LutSize};
    std::uint32_t channels{2};
    std::uint32_t samples{SampleCount};
};
static_assert(sizeof(Header) == 24);
}

int main(const int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: GenerateBRDFLut <output-file>\n";
        return 2;
    }
    try {
        const std::filesystem::path output = argv[1];
        std::filesystem::create_directories(output.parent_path());
        const auto pixels = Engine::EnvironmentBaker::generateBrdfLut(LutSize, SampleCount);
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("could not open output");
        const Header header{};
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        stream.write(reinterpret_cast<const char*>(pixels.data()),
                     static_cast<std::streamsize>(pixels.size() * sizeof(pixels.front())));
        if (!stream) throw std::runtime_error("could not write output");
        std::cout << "Generated " << output.string() << " (BRDF v" << BrdfVersion << ")\n";
    } catch (const std::exception& error) {
        std::cerr << "GenerateBRDFLut: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
