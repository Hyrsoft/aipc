#include "audio_ipc_publisher.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
void Expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int main() {
    media_worker::EncodedAudioFrame frame{{1, 2, 3}, 0x0102030405060708ULL,
                                           0x1112131415161718ULL};
    const auto encoded = media_worker::EncodeAudioIpcFrame(frame);
    Expect(encoded.size() == media_worker::kAudioIpcHeaderSize + 3, "encoded size");
    Expect(encoded[0] == 'A' && encoded[3] == 'A', "magic");
    Expect(encoded[5] == 1 && encoded[11] == 3, "version and payload length");

    std::array<int, 2> sockets{};
    Expect(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0, "socketpair");
    media_worker::AudioIpcPublisher publisher(sockets[0], 2);
    Expect(publisher.Start(), "publisher starts");
    Expect(publisher.Enqueue(frame), "frame accepted");
    pollfd descriptor{sockets[1], POLLIN, 0};
    Expect(poll(&descriptor, 1, 1000) == 1, "publisher becomes readable");
    std::array<std::uint8_t, 64> buffer{};
    const ssize_t received = read(sockets[1], buffer.data(), buffer.size());
    Expect(received == static_cast<ssize_t>(encoded.size()), "framed payload received");
    publisher.Stop();
    close(sockets[1]);
    Expect(publisher.Frames() == 1, "published frame counted");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
