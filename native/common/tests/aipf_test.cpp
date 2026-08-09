#include "aipc/native/aipf.h"
#include "aipc/native/aipv2.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>
#include <unistd.h>

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

    aipc::native::EncodedAccessUnit access_unit;
    access_unit.data = {0, 0, 0, 1, 0x65, 0x88};
    access_unit.pts = 123456;
    access_unit.sequence = 42;
    access_unit.flags = aipc::native::kAipv2Keyframe |
                        aipc::native::kAipv2CodecConfig;
    const auto encoded_access_unit =
        aipc::native::EncodeAipv2AccessUnit(access_unit);
    assert(encoded_access_unit.size() ==
           aipc::native::kAipv2HeaderSize + access_unit.data.size());
    assert(encoded_access_unit[0] == 'A' && encoded_access_unit[3] == 'V');
    assert(encoded_access_unit[4] == 0 && encoded_access_unit[5] == 2);
    assert(encoded_access_unit[7] == 5);
    int fds[2];
    assert(pipe(fds) == 0);
    assert(write(fds[1], encoded_access_unit.data(), encoded_access_unit.size()) ==
           static_cast<ssize_t>(encoded_access_unit.size()));
    close(fds[1]);
    std::string protocol_error;
    const auto decoded_access_unit =
        aipc::native::ReadAipv2AccessUnit(fds[0], &protocol_error);
    close(fds[0]);
    assert(decoded_access_unit.has_value());
    assert(protocol_error.empty());
    assert(decoded_access_unit->data == access_unit.data);
    assert(decoded_access_unit->pts == access_unit.pts);
    assert(decoded_access_unit->sequence == access_unit.sequence);
    assert(decoded_access_unit->keyframe());
    return 0;
}
