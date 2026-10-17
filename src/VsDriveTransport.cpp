// POM2 Apple II Emulator — GPL-3.0-or-later
#include "VsDriveTransport.h"

#include "SuperSerialCard.h"
#include "ThreadGuard.h"
#include "VsDriveProtocol.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace pom2 {
namespace {

class VsDriveTransport final : public SuperSerialTransport
{
public:
    explicit VsDriveTransport(SuperSerialCard& card) : card_(card) {}
    ~VsDriveTransport() override { stop(); }

    bool prepare(const std::array<std::string, 2>& paths,
                 const std::array<bool, 2>& readOnly, std::string& error)
    {
        return protocol_.open(paths, readOnly, error);
    }

    bool start(uint16_t) override
    {
        if (running_) return true;
        protocol_.resetPacket();
        stopRequested_ = false;
        running_ = true;
        card_.onTransportConnected();
        worker_ = guardedThread("VSDrive", [this] { run(); });
        return true;
    }

    void stop() override
    {
        if (!running_ && !worker_.joinable()) return;
        stopRequested_ = true;
        if (worker_.joinable()) worker_.join();
        card_.onTransportDisconnected();
        running_ = false;
    }

    bool isListening() const override { return running_; }
    uint16_t port() const override { return 0; }
    bool isVsDrive() const override { return true; }
    std::string vsDrivePath(int drive) const override
    {
        return drive >= 0 && drive < 2 ? protocol_.paths()[drive] : std::string{};
    }
    bool vsDriveReadOnly(int drive) const override
    {
        return drive >= 0 && drive < 2 && protocol_.readOnly()[drive];
    }

private:
    void run()
    {
        std::vector<uint8_t> incoming;
        std::vector<uint8_t> replies;
        while (!stopRequested_) {
            incoming.clear();
            card_.drainTransportTx(incoming);
            if (!incoming.empty()) {
                replies.clear();
                protocol_.feed(incoming.data(), incoming.size(), replies);
                if (!replies.empty())
                    card_.deliverTransportBytes(replies.data(), replies.size(), false);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    SuperSerialCard& card_;
    VsDriveProtocol protocol_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace

std::unique_ptr<SuperSerialTransport> makeVsDriveTransport(
    SuperSerialCard& card, const std::array<std::string, 2>& paths,
    std::string& error, const std::array<bool, 2>& readOnly)
{
    auto transport = std::make_unique<VsDriveTransport>(card);
    if (!transport->prepare(paths, readOnly, error)) return {};
    return transport;
}

} // namespace pom2
