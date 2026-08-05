#include "video_ipc_publisher.h"

#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestHeader() {
    media_worker::EncodedVideoFrame frame{{1, 2, 3}, 0x0102030405060708ULL,
                                          0x1112131415161718ULL, true};
    const auto encoded = media_worker::EncodeVideoIpcFrame(frame);
    Expect(encoded.size() == media_worker::kVideoIpcHeaderSize + 3, "encoded size");
    Expect(encoded[0] == 'A' && encoded[3] == 'V', "magic");
    Expect(encoded[5] == 1 && encoded[7] == 1, "version and keyframe flag");
    Expect(encoded[11] == 3, "payload length");
    Expect(encoded[12] == 1 && encoded[19] == 8, "PTS big endian");
    Expect(encoded[20] == 0x11 && encoded[27] == 0x18, "sequence big endian");
}

void TestQueueRecovery() {
    media_worker::VideoFrameQueue queue(2);
    std::atomic<bool> running{true};
    Expect(queue.Push({{1}, 1, 1, false}).accepted, "first delta accepted");
    Expect(queue.Push({{2}, 2, 2, false}).accepted, "second delta accepted");
    const auto overflow = queue.Push({{3}, 3, 3, false});
    Expect(overflow.request_idr && overflow.dropped == 3, "overflow requests IDR");
    Expect(!queue.Push({{4}, 4, 4, false}).accepted, "delta dropped until keyframe");
    Expect(queue.Push({{5}, 5, 5, true}).accepted, "keyframe restores queue");
    media_worker::EncodedVideoFrame output;
    Expect(queue.WaitPop(&output, running) && output.keyframe, "restored keyframe popped");
    queue.Stop();
}

void TestSocketWriteAndDisconnect() {
    std::array<int, 2> sockets{};
    Expect(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0, "socketpair");
    std::string error;
    media_worker::VideoIpcPublisher publisher(
        sockets[0], 8, [&](const std::string& value) { error = value; });
    Expect(publisher.Start(), "publisher starts");
    publisher.Enqueue({{0, 0, 0, 1, 0x65, 9}, 42, 1, true});
    std::array<std::uint8_t, 64> buffer{};
    pollfd descriptor{sockets[1], POLLIN, 0};
    const int poll_result = poll(&descriptor, 1, 1000);
    Expect(poll_result == 1, "publisher becomes readable");
    const ssize_t received =
        poll_result == 1 ? read(sockets[1], buffer.data(), buffer.size()) : -1;
    Expect(received == static_cast<ssize_t>(media_worker::kVideoIpcHeaderSize + 6),
           "framed payload received");
    close(sockets[1]);
    publisher.Enqueue({std::vector<std::uint8_t>(1024, 7), 43, 2, false});
    usleep(20000);
    publisher.Stop();
    Expect(publisher.Frames() >= 1, "published frame counted");
}

}  // namespace

int main() {
    TestHeader();
    TestQueueRecovery();
    TestSocketWriteAndDisconnect();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "all video IPC tests passed\n";
    return EXIT_SUCCESS;
}
