// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#ifndef POM2_TEST_FAKE_W5100_SOCKET_H
#define POM2_TEST_FAKE_W5100_SOCKET_H

#include "W5100Socket.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pom2::test {

class FakeW5100HostSocket final : public W5100HostSocket
{
public:
    struct Packet {
        W5100ReceiveResult result;
        std::vector<uint8_t> data;
    };

    W5100ConnectResult connect(uint32_t address, uint16_t port) override
    {
        ++connectCount;
        // …and on the FACTORY too, which outlives us. `lastSocket` is a bare
        // observer into a unique_ptr the device owns, so any test that asks
        // "was connect() attempted?" *after* the device closed the socket —
        // which is exactly what the loopback-refusal path does — was reading
        // freed memory. ASan caught it nightly; a tally that survives the
        // socket cannot be read too late.
        if (factoryConnectTally) ++*factoryConnectTally;
        lastConnectAddress = address;
        lastConnectPort = port;
        return connectResult;
    }

    W5100ConnectResult pollConnect() override
    {
        ++pollConnectCount;
        return pollConnectResult;
    }

    bool writable() const override { return writableResult; }

    bool bind(uint16_t port) override
    {
        ++bindCount;
        lastBindPort = port;
        return bindResult;
    }

    W5100ReceiveResult receive(uint8_t* data, std::size_t capacity) override
    {
        ++receiveCount;
        if (packets.empty()) return {W5100IoStatus::WouldBlock, 0, 0, 0};
        Packet packet = std::move(packets.front());
        packets.pop_front();
        const std::size_t copied = std::min(capacity, packet.data.size());
        if (copied) std::memcpy(data, packet.data.data(), copied);
        packet.result.bytes = copied;
        return packet.result;
    }

    W5100SendResult send(const uint8_t* data, std::size_t size,
                         uint32_t address, uint16_t port,
                         W5100SendMode mode) override
    {
        ++sendCount;
        lastSendAddress = address;
        lastSendPort = port;
        lastSendMode = mode;
        if (data && size) sentBytes.insert(sentBytes.end(), data, data + size);
        if (sendResult.status == W5100IoStatus::Ok && sendResult.bytes == 0)
            return {W5100IoStatus::Ok, size};
        return sendResult;
    }

    W5100ConnectResult connectResult = W5100ConnectResult::Connected;
    W5100ConnectResult pollConnectResult = W5100ConnectResult::Connected;
    W5100SendResult sendResult{W5100IoStatus::Ok, 0};
    std::deque<Packet> packets;
    std::vector<uint8_t> sentBytes;
    uint32_t lastConnectAddress = 0;
    uint16_t lastConnectPort = 0;
    uint32_t lastSendAddress = 0;
    uint16_t lastSendPort = 0;
    bool writableResult = true;
    W5100SendMode lastSendMode = W5100SendMode::Addressed;
    bool bindResult = true;
    uint16_t lastBindPort = 0;
    int bindCount = 0;
    int connectCount = 0;
    int* factoryConnectTally = nullptr;
    int pollConnectCount = 0;
    int receiveCount = 0;
    int sendCount = 0;
};

class FakeW5100SocketFactory final : public W5100SocketFactory
{
public:
    std::unique_ptr<W5100HostSocket> open(W5100SocketKind kind) override
    {
        ++openCount;
        lastKind = kind;
        auto socket = std::make_unique<FakeW5100HostSocket>();
        socket->connectResult = nextConnectResult;
        socket->pollConnectResult = nextPollConnectResult;
        socket->factoryConnectTally = &connectAttempts;
        lastSocket = socket.get();
        return socket;
    }




    W5100ConnectResult nextConnectResult = W5100ConnectResult::Connected;
    W5100ConnectResult nextPollConnectResult = W5100ConnectResult::Connected;
    uint32_t resolveResult = 0;
    /// DANGLES once the device closes the socket it points at — the device
    /// owns those. Read it only while the socket is still open; for anything
    /// asked afterwards use `connectAttempts`, which lives here.
    FakeW5100HostSocket* lastSocket = nullptr;
    /// Every connect() any socket this factory made has attempted.
    int connectAttempts = 0;
    W5100SocketKind lastKind = W5100SocketKind::Tcp;
    std::string lastHostname;
    int lastResolveWaitMs = 0;
    int openCount = 0;
    int resolveCount = 0;
    int pollResolverCount = 0;
    int clearResolverCacheCount = 0;
};

} // namespace pom2::test

#endif // POM2_TEST_FAKE_W5100_SOCKET_H
