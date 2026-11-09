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

// Insert-path stack-depth smoke test.
//
// Pins the 2026-08-23 arm64-macOS SIGBUS: sizeof(DiskImage) is ~242 KB
// (the track buffers live in the object), and the insert path used to stack
// one temporary per frame — insertDisk → prepareDisk → loadFile — for ~725 KB
// of frames. Linux gives a std::thread 8 MB of stack and never noticed;
// macOS gives it 512 KB and ___chkstk_darwin's probe walked off the guard
// page. The AI control server's HTTP thread reaches insertDisk exactly like
// this, so the crash was a real app crash, not a test artefact. The fix
// heap-allocated the temporaries. STORAGE-003 now moves the track storage
// itself off the stack; direct value copies below cover the whole class.
//
// To make the regression fail on EVERY platform rather than only where the
// default thread stack happens to be small, the insert runs here on a pthread
// whose stack is explicitly 512 KB — the macOS default, requested portably.
// On Windows there is no pthread; the std::thread fallback still pins the
// path on that platform's 1 MB default, which the pre-fix code also exceeded
// once the caller's own frames are counted.

#include "DiskIICard.h"
#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#else
#include <thread>
#endif

namespace fs = std::filesystem;

namespace {

fs::path tmpDsk(const std::string& tag)
{
    fs::path p = fs::temp_directory_path() / ("pom2_insert_stack_" + tag);
    std::vector<uint8_t> bytes(DiskImage::kBytesPerImage, 0);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFF);
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return p;
}

struct InsertJob {
    DiskIICard* card;
    const std::string* pathA;
    const std::string* pathB;
    bool ok = false;
};

// The deepest mount chains a thread actually runs: the inline insert
// (insertDisk → prepareDisk → loadFile) and the two-phase install's
// same-file-still-dirty re-read (installDisk → prepareDisk → loadFile).
void runInserts(InsertJob& job)
{
    // Three values alone exceeded 512 KiB before STORAGE-003. Check actual
    // nibble content, copy independence, move and reuse through the loader.
    static_assert(sizeof(DiskImage) < 32 * 1024, "track buffers returned to the stack");
    DiskImage first;
    first.setWriteBackEnabled(false);
    assert(first.loadFile(*job.pathA));
    DiskImage second(first);
    const auto original = first.nibbleAt(0, 0);
    second.setWriteBackEnabled(true);
    second.writeNibbleAt(0, 0, original ^ 1);
    assert(first.nibbleAt(0, 0) == original);
    assert(second.nibbleAt(0, 0) == (original ^ 1));
    DiskImage third(std::move(second));
    assert(third.nibbleAt(0, 0) == (original ^ 1));
    assert(second.loadFile(*job.pathB));
    assert(second.nibbleAt(0, 0) == original);
    third = first;
    assert(third.nibbleAt(0, 0) == original);
    third = std::move(second);
    assert(third.isLoaded());
    second.eject();
    assert(second.loadFile(*job.pathA));

    bool ok = job.card->insertDisk(0, *job.pathA);
    ok = ok && job.card->insertDisk(0, *job.pathB);
    ok = ok && job.card->ejectDisk(0);
    ok = ok && job.card->insertDisk(0, *job.pathA);
    job.ok = ok;
}

#ifndef _WIN32
void* threadMain(void* arg)
{
    runInserts(*static_cast<InsertJob*>(arg));
    return nullptr;
}
#endif

}  // namespace

int main()
{
    const std::string a = tmpDsk("a.dsk").string();
    const std::string b = tmpDsk("b.dsk").string();

    DiskIICard card(6);
    InsertJob job{&card, &a, &b};

#ifndef _WIN32
    // 512 KB — the macOS secondary-thread default, forced everywhere so the
    // pin bites on Linux CI too. PTHREAD_STACK_MIN is far below this on every
    // supported platform, so no clamp is needed.
    constexpr size_t kStackBytes = 512 * 1024;
    pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, kStackBytes) == 0);
    pthread_t th;
    assert(pthread_create(&th, &attr, threadMain, &job) == 0);
    pthread_attr_destroy(&attr);
    assert(pthread_join(th, nullptr) == 0);
#else
    std::thread th([&job] { runInserts(job); });
    th.join();
#endif

    assert(job.ok);
    assert(card.isDiskLoaded(0));
    assert(card.getDiskPath(0) == a);

    std::error_code ec;
    fs::remove(a, ec);
    fs::remove(b, ec);

    std::printf("diskii_insert_thread_stack_test: OK\n");
    return 0;
}
