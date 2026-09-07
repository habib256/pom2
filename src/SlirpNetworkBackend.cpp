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

// SlirpNetworkBackend implementation. See the header for the rationale
// and the virtual-network layout.

#include "SlirpNetworkBackend.h"

#include "Logger.h"

#ifdef POM2_HAVE_SLIRP

// libslirp's pkg-config puts `<prefix>/include/slirp` on the include path,
// so the canonical spelling is the bare <libslirp.h>. Debian/Ubuntu also
// happen to resolve <slirp/libslirp.h> because their prefix is the default
// /usr, but Homebrew's is not — hence the probe rather than either form.
#if defined(__has_include)
#  if __has_include(<libslirp.h>)
#    include <libslirp.h>
#  else
#    include <slirp/libslirp.h>
#  endif
#else
#  include <libslirp.h>
#endif

#include <arpa/inet.h>
#include <poll.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <vector>

namespace pom2 {
namespace {

// libslirp calls back into us for timers. It only ever asks for a couple,
// so a flat vector of heap nodes is plenty — no need for a priority queue.
struct SlirpTimer {
    SlirpTimerCb cb        = nullptr;
    void*        cbOpaque  = nullptr;
    int64_t      expireMs  = -1;   // -1 = disarmed
    bool         dead      = false;
};

class SlirpBackend final : public NetworkBackend
{
public:
    /// Same backstop as MAME's CS8900A inbound queue
    /// (`machine/cs8900a.cpp:39`): drop the oldest rather than grow
    /// without bound when the guest stops draining.
    static constexpr size_t kMaxQueued = 4096;

    SlirpBackend(const std::string& hostname, const SlirpOptions& options)
    {
        SlirpConfig cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        // The socket polling API arrived with libslirp 4.9 / config v6.
        // Older supported releases retain the fd API below.
#if SLIRP_CHECK_VERSION(4, 9, 0)
        cfg.version    = 6;
#else
        cfg.version    = 1;
#endif
        cfg.restricted = options.restricted ? 1 : 0;
        cfg.in_enabled = true;
        cfg.vnetwork.s_addr     = htonl(0x0A000200);  // 10.0.2.0
        cfg.vnetmask.s_addr     = htonl(0xFFFFFF00);  // /24
        cfg.vhost.s_addr        = htonl(0x0A000202);  // 10.0.2.2 gateway
        cfg.vdhcp_start.s_addr  = htonl(0x0A00020F);  // 10.0.2.15
        cfg.vnameserver.s_addr  = htonl(0x0A000203);  // 10.0.2.3
        cfg.in6_enabled = false;
        cfg.vhostname   = hostname.empty() ? nullptr : hostname.c_str();
        // MTU/MRU 0 = libslirp's defaults (1500), which is what an
        // Ethernet-attached Apple II expects.
        cfg.if_mtu = 0;
        cfg.if_mru = 0;
        // TRUE by default — see SlirpOptions::allowHostLoopback. This is the
        // one line that stops a guest with its own IP stack from reaching the
        // host's private services through the virtual router at 10.0.2.2.
        cfg.disable_host_loopback = !options.allowHostLoopback;
        cfg.enable_emu = false;

        static const SlirpCb kCallbacks = [] {
            SlirpCb cb{};
            cb.send_packet  = &SlirpBackend::cbSendPacket;
            cb.guest_error  = &SlirpBackend::cbGuestError;
            cb.clock_get_ns = &SlirpBackend::cbClockGetNs;
            cb.timer_new    = &SlirpBackend::cbTimerNew;
            cb.timer_free   = &SlirpBackend::cbTimerFree;
            cb.timer_mod    = &SlirpBackend::cbTimerMod;
            cb.notify       = &SlirpBackend::cbNotify;
#if SLIRP_CHECK_VERSION(4, 9, 0)
            cb.register_poll_socket   = &SlirpBackend::cbRegisterPollSocket;
            cb.unregister_poll_socket = &SlirpBackend::cbUnregisterPollSocket;
#else
            cb.register_poll_fd   = &SlirpBackend::cbRegisterPollFd;
            cb.unregister_poll_fd = &SlirpBackend::cbUnregisterPollFd;
#endif
            return cb;
        }();

        slirp_ = slirp_new(&cfg, &kCallbacks, this);
        if (!slirp_) {
            log().error("Slirp", "slirp_new failed — Ethernet backend unavailable");
            return;
        }
        name_ = std::string("libslirp (guest 10.0.2.15, gw 10.0.2.2, "
                            "dns 10.0.2.3") +
                (options.restricted ? ", restricted" : "") +
                (options.allowHostLoopback ? ", host loopback ALLOWED" : "") + ")";
        log().info("Slirp", "user-mode NAT up — " + name_);
    }

    ~SlirpBackend() override
    {
        if (slirp_) slirp_cleanup(slirp_);
        for (SlirpTimer* t : timers_) delete t;
    }

    bool isValid() const override { return slirp_ != nullptr; }
    std::string_view name() const override { return name_; }

    void transmit(const uint8_t* frame, int len) override
    {
        if (!slirp_ || len < kMinEthFrame || len > kMaxEthFrame) {
            ++framesDropped_;
            return;
        }
        slirp_input(slirp_, frame, len);
        ++framesSent_;
    }

    int receive(uint8_t* buf, int cap) override
    {
        if (rxQueue_.empty()) return 0;
        const std::vector<uint8_t>& f = rxQueue_.front();
        const int n = static_cast<int>(f.size());
        if (n > cap) { rxQueue_.pop_front(); ++framesDropped_; return 0; }
        std::memcpy(buf, f.data(), static_cast<size_t>(n));
        rxQueue_.pop_front();
        ++framesReceived_;
        return n;
    }

    void poll() override
    {
        if (!slirp_) return;

        // Fire any timer whose deadline has passed. libslirp arms these
        // for its own retransmit / RA housekeeping.
        const int64_t nowMs = nowNs() / 1000000;
        for (size_t i = 0; i < timers_.size(); ++i) {
            SlirpTimer* t = timers_[i];
            if (t->dead || t->expireMs < 0 || t->expireMs > nowMs) continue;
            t->expireMs = -1;
            if (t->cb) t->cb(t->cbOpaque);
        }
        reapDeadTimers();

        // One non-blocking poll round: slirp tells us which fds it cares
        // about (fill), we poll them with a zero timeout, then hand the
        // results back (poll). Zero timeout is mandatory — this runs on
        // the CPU thread under stateMutex.
        pollFds_.clear();
        uint32_t timeoutMs = 0;
#if SLIRP_CHECK_VERSION(4, 9, 0)
        slirp_pollfds_fill_socket(slirp_, &timeoutMs,
                                  &SlirpBackend::cbAddPollSocket, this);
#else
        slirp_pollfds_fill(slirp_, &timeoutMs, &SlirpBackend::cbAddPollFd, this);
#endif

        int rc = 0;
        if (!pollFds_.empty())
            rc = ::poll(pollFds_.data(), pollFds_.size(), 0);

        slirp_pollfds_poll(slirp_, rc < 0 ? 1 : 0, &SlirpBackend::cbGetREvents, this);
    }

    /// slirp answers ARP for everything on its virtual network with the
    /// 52:55:xx:xx:xx:xx form derived from the IP (see libslirp
    /// `src/arp_table.c` / `slirp.c: special addresses`). Synthesising it
    /// here saves an ARP round-trip that the guest never sees anyway,
    /// and matches what slirp would have replied.
    bool resolveMac(uint32_t ipv4NetworkOrder, MacAddress& out) override
    {
        if (!slirp_) return false;
        const uint8_t* ip = reinterpret_cast<const uint8_t*>(&ipv4NetworkOrder);
        out.b[0] = 0x52; out.b[1] = 0x55;
        out.b[2] = ip[0]; out.b[3] = ip[1]; out.b[4] = ip[2]; out.b[5] = ip[3];
        return true;
    }

private:
    static int64_t nowNs()
    {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    }

    void reapDeadTimers()
    {
        for (size_t i = 0; i < timers_.size();) {
            if (timers_[i]->dead) {
                delete timers_[i];
                timers_[i] = timers_.back();
                timers_.pop_back();
            } else {
                ++i;
            }
        }
    }

    // ── libslirp callbacks ────────────────────────────────────────────
    // All take the `opaque` we passed to slirp_new — i.e. `this`.

    static ssize_t cbSendPacket(const void* buf, size_t len, void* opaque)
    {
        auto* self = static_cast<SlirpBackend*>(opaque);
        if (len < static_cast<size_t>(kMinEthFrame) ||
            len > static_cast<size_t>(kMaxEthFrame)) {
            ++self->framesDropped_;
            return static_cast<ssize_t>(len);   // consumed-and-dropped, not an error
        }
        if (self->rxQueue_.size() >= kMaxQueued) {
            self->rxQueue_.pop_front();
            ++self->framesDropped_;
        }
        const auto* p = static_cast<const uint8_t*>(buf);
        self->rxQueue_.emplace_back(p, p + len);
        return static_cast<ssize_t>(len);
    }

    static void cbGuestError(const char* msg, void* /*opaque*/)
    {
        log().warn("Slirp", msg ? msg : "(guest error)");
    }

    static int64_t cbClockGetNs(void* /*opaque*/) { return nowNs(); }

    static void* cbTimerNew(SlirpTimerCb cb, void* cbOpaque, void* opaque)
    {
        auto* self = static_cast<SlirpBackend*>(opaque);
        auto* t = new SlirpTimer{ cb, cbOpaque, -1, false };
        self->timers_.push_back(t);
        return t;
    }

    static void cbTimerFree(void* timer, void* /*opaque*/)
    {
        // Deferred: timer_free can be called from inside a timer
        // callback, so we must not delete out from under poll()'s loop.
        if (auto* t = static_cast<SlirpTimer*>(timer)) t->dead = true;
    }

    static void cbTimerMod(void* timer, int64_t expireTimeMs, void* /*opaque*/)
    {
        if (auto* t = static_cast<SlirpTimer*>(timer)) t->expireMs = expireTimeMs;
    }

#if SLIRP_CHECK_VERSION(4, 9, 0)
    static void cbRegisterPollSocket(slirp_os_socket /*fd*/, void* /*opaque*/) {}
    static void cbUnregisterPollSocket(slirp_os_socket /*fd*/, void* /*opaque*/) {}
#else
    static void cbRegisterPollFd(int /*fd*/, void* /*opaque*/) {}
    static void cbUnregisterPollFd(int /*fd*/, void* /*opaque*/) {}
#endif
    static void cbNotify(void* /*opaque*/) {}

    static int addPollFd(int fd, int events, void* opaque)
    {
        auto* self = static_cast<SlirpBackend*>(opaque);
        pollfd p{};
        p.fd = fd;
        p.events = 0;
        if (events & SLIRP_POLL_IN)  p.events |= POLLIN;
        if (events & SLIRP_POLL_OUT) p.events |= POLLOUT;
        if (events & SLIRP_POLL_PRI) p.events |= POLLPRI;
        if (events & SLIRP_POLL_ERR) p.events |= POLLERR;
        if (events & SLIRP_POLL_HUP) p.events |= POLLHUP;
        self->pollFds_.push_back(p);
        return static_cast<int>(self->pollFds_.size()) - 1;
    }

#if SLIRP_CHECK_VERSION(4, 9, 0)
    static int cbAddPollSocket(slirp_os_socket fd, int events, void* opaque)
    {
        return addPollFd(fd, events, opaque);
    }
#else
    static int cbAddPollFd(int fd, int events, void* opaque)
    {
        return addPollFd(fd, events, opaque);
    }
#endif

    static int cbGetREvents(int idx, void* opaque)
    {
        auto* self = static_cast<SlirpBackend*>(opaque);
        if (idx < 0 || static_cast<size_t>(idx) >= self->pollFds_.size()) return 0;
        const short r = self->pollFds_[static_cast<size_t>(idx)].revents;
        int events = 0;
        if (r & POLLIN)  events |= SLIRP_POLL_IN;
        if (r & POLLOUT) events |= SLIRP_POLL_OUT;
        if (r & POLLPRI) events |= SLIRP_POLL_PRI;
        if (r & POLLERR) events |= SLIRP_POLL_ERR;
        if (r & POLLHUP) events |= SLIRP_POLL_HUP;
        return events;
    }

    Slirp*                           slirp_ = nullptr;
    std::string                      name_  = "libslirp (down)";
    std::deque<std::vector<uint8_t>> rxQueue_;
    std::vector<pollfd>              pollFds_;
    std::vector<SlirpTimer*>         timers_;
};

} // namespace

bool slirpAvailable() { return true; }

std::unique_ptr<NetworkBackend> makeSlirpBackend(const std::string& hostname,
                                                 const SlirpOptions& options)
{
    auto b = std::make_unique<SlirpBackend>(hostname, options);
    if (!b->isValid()) return nullptr;
    return b;
}

} // namespace pom2

#else  // !POM2_HAVE_SLIRP

namespace pom2 {

bool slirpAvailable() { return false; }

std::unique_ptr<NetworkBackend> makeSlirpBackend(const std::string&,
                                                 const SlirpOptions&)
{
    return nullptr;
}

} // namespace pom2

#endif // POM2_HAVE_SLIRP
