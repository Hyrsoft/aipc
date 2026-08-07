#include "aipc/native/aipf.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace {

std::vector<std::uint8_t> DecodeHex(const std::string& value) {
    assert(value.size() % 2 == 0);
    std::vector<std::uint8_t> output;
    output.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        output.push_back(static_cast<std::uint8_t>(
            std::stoul(value.substr(index, 2), nullptr, 16)));
    }
    return output;
}

}  // namespace

int main() {
    std::ifstream input(std::string(AIPC_PROTOCOL_FIXTURE_DIR) + "/aipf-v1.json");
    assert(input.good());
    const auto fixture = nlohmann::json::parse(input);
    aipc::native::AiFrame frame;
    frame.data = DecodeHex(fixture.at("payload_hex"));
    frame.pts = fixture.at("pts");
    frame.sequence = fixture.at("sequence");
    frame.width = fixture.at("width");
    frame.height = fixture.at("height");
    frame.y_stride = fixture.at("y_stride");
    frame.uv_stride = fixture.at("uv_stride");
    frame.height_stride = fixture.at("height_stride");
    frame.main_width = fixture.at("main_width");
    frame.main_height = fixture.at("main_height");
    frame.fit_mode = aipc::native::AiFitMode::kContain;
    const auto& transform = fixture.at("transform");
    frame.transform.crop_x = transform.at("crop_x");
    frame.transform.crop_y = transform.at("crop_y");
    frame.transform.crop_width = transform.at("crop_width");
    frame.transform.crop_height = transform.at("crop_height");
    frame.transform.pad_left = transform.at("pad_left");
    frame.transform.pad_top = transform.at("pad_top");
    frame.transform.pad_right = transform.at("pad_right");
    frame.transform.pad_bottom = transform.at("pad_bottom");
    const auto encoded = aipc::native::EncodeAipfFrame(frame);
    assert(encoded == DecodeHex(fixture.at("encoded_hex")));
    assert(encoded.size() == aipc::native::kAipfHeaderSize + frame.data.size());
    assert(encoded[0] == 'A' && encoded[1] == 'I' && encoded[2] == 'P' && encoded[3] == 'F');
    assert(encoded[4] == 0 && encoded[5] == 1);
    assert(encoded[6] == 0 && encoded[7] == 1);
    assert(encoded[12] == 1 && encoded[19] == 8);
    return 0;
}
