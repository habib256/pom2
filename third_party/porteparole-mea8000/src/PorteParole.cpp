// SPDX-License-Identifier: GPL-2.0-or-later
#include "StdAfx.h"
#include "PorteParole.h"
#include "CardManager.h"
#include "Core.h"
#include "CPU.h"
#include "Memory.h"
#include "YamlHelper.h"

PorteParoleCard::PorteParoleCard(UINT slot) : Card(CT_PorteParole, slot), m_lastCycle(g_nCumulativeCycles)
{
    if (slot < SLOT1 || slot > SLOT7) ThrowErrorInvalidSlot();
    m_audio.reserve(4096);
}

void PorteParoleCard::InitializeIO(LPBYTE)
{
    // Standard board has no optional 2716/6116 fitted: do not publish a fictitious ROM.
    // A0-only register mirroring is a provisional hardware assumption (manual annex 5).
    RegisterIoHandler(m_slot, IO, IO, IO_Null, IO_Null, this, NULL);
}

void PorteParoleCard::Destroy()
{
    if (m_audioTrace) { fclose(m_audioTrace); m_audioTrace = nullptr; }
    if (m_voice.lpDSBvoice) DSReleaseSoundBuffer(&m_voice);
    m_voice.bActive = false; m_offset = UINT32_MAX; m_audio.clear(); m_preventTurboUntil = 0;
}

void PorteParoleCard::Reset(bool /*powerCycle*/)
{
    m_mea.Reset(); m_clockFraction = 0; m_lastCycle = g_nCumulativeCycles;
    Destroy();
}

void PorteParoleCard::Synchronize()
{
    const UINT64 now = g_nCumulativeCycles;
    if (now < m_lastCycle) { m_lastCycle = now; return; }
    const UINT64 delta = now - m_lastCycle; m_lastCycle = now;
    double exact = m_clockFraction + double(delta) * Mea8000::Clock / g_fCurrentCLK6502;
    const UINT64 clocks = static_cast<UINT64>(exact);
    m_clockFraction = exact - double(clocks);
    const bool wasActive = m_mea.Active();
    m_mea.AdvanceClocks(clocks, g_bFullSpeed ? nullptr : &m_audio);
    if (wasActive || m_mea.Active())
        m_preventTurboUntil = now + static_cast<UINT64>(g_fCurrentCLK6502 * 0.3);
    // Bound host queue even if audio device fails or the emulator is temporarily unpaced.
    if (m_audio.size() > Mea8000::OutputRate / 5) m_audio.clear();
}

BYTE __stdcall PorteParoleCard::IO(WORD, WORD addr, BYTE write, BYTE value, ULONG cycles)
{
    auto* card = static_cast<PorteParoleCard*>(MemGetSlotParameters(((addr & 0xff) >> 4) - 8));
    // This is the timed device itself, not an observational probe. AppleWin documents
    // CpuCalcCycles as the way to obtain the current absolute IO cycle.
    CpuCalcCycles(cycles);
    card->Synchronize();
    if (write)
    {
        if (addr & 1) card->m_mea.WriteCommand(value);
        else card->m_mea.WriteData(value);
        return 0;
    }
    // Only D7 is established. Lower bits are provisionally zero, matching the MEA reference.
    return card->m_mea.ReadStatus();
}

void PorteParoleCard::Update(ULONG)
{
    Synchronize();
    FlushAudio();
}

void PorteParoleCard::FlushAudio()
{
    if (g_bFullSpeed)
    {
        if (!m_wasFullSpeed) TraceAudio("turbo");
        if (m_voice.lpDSBvoice && m_voice.bActive) DSVoiceStop(&m_voice);
        m_audio.clear(); m_offset = UINT32_MAX; m_wasFullSpeed = true; return;
    }
    if (!m_voice.lpDSBvoice)
    {
        if (!m_mea.Active()) { m_audio.clear(); return; }
        if (FAILED(DSGetSoundBuffer(&m_voice, BufferBytes, Mea8000::OutputRate, 1, "Porte-Parole")))
        { m_audio.clear(); return; }
    }
    if (!m_voice.bActive || m_wasFullSpeed)
    {
        if (!DSZeroVoiceBuffer(&m_voice, BufferBytes)) { m_audio.clear(); return; }
        m_offset = UINT32_MAX; m_wasFullSpeed = false;
    }
    if (m_audio.empty()) return;
    DWORD play, write;
    if (FAILED(m_voice.lpDSBvoice->GetCurrentPosition(&play, &write))) { m_audio.clear(); return; }
    if (m_offset == UINT32_MAX)
        // 100 ms lead absorbs normal Windows scheduling jitter. In particular the
        // backend may initially report both cursors at zero for several updates.
        m_offset = (write + BufferBytes / 2) % BufferBytes;
    // Equal cursors denote an empty interval, NOT a wrapped unsafe interval.
    // The common helper's wrap branch rebased every positive offset to zero in
    // this startup case, destroying the lead and creating repeated discontinuities.
    else if (play != write && SoundCore_ValidateAndAlignWriteOffset(m_offset, play, write))
    {
        TraceAudio("underrun-align", play, write);
        m_offset = (write + BufferBytes / 3) % BufferBytes;
    }
    m_offset &= ~1U;
    const uint32_t size = static_cast<uint32_t>(m_audio.size()) * sizeof(int16_t);
    // Keep host playback safely behind write head without overwriting unread audio.
    const uint32_t safe = (play + BufferBytes - m_offset - 2) % BufferBytes;
    if (size > safe) { TraceAudio("overflow-drop",play,write); m_audio.clear(); m_offset = UINT32_MAX; return; }
    SHORT *p0 = nullptr, *p1 = nullptr; DWORD n0 = 0, n1 = 0;
    if (FAILED(DSGetLock(m_voice.lpDSBvoice, m_offset, size, &p0, &n0, &p1, &n1)))
    { m_audio.clear(); return; }
    TraceAudio("submit",play,write);
    memcpy(p0, m_audio.data(), n0);
    if (n1) memcpy(p1, reinterpret_cast<const BYTE*>(m_audio.data()) + n0, n1);
    m_voice.lpDSBvoice->Unlock(p0, n0, p1, n1);
    m_offset = (m_offset + size) % BufferBytes;
    m_audio.clear();
}

void PorteParoleCard::TraceAudio(const char* event, DWORD play, DWORD write)
{
    // Explicit, bounded diagnostics for one controlled session. No files by default.
    if (!m_traceAttempted)
    {
        m_traceAttempted = true;
        char path[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("PP_AUDIO_TRACE", path, MAX_PATH);
        if (n && n < MAX_PATH && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            fopen_s(&m_audioTrace, path, "wx");
        if (m_audioTrace)
        {
            setvbuf(m_audioTrace, NULL, _IOFBF, 65536);
            fprintf(m_audioTrace,"tick_ms,cycles,event,play,write,offset,frames,active,fullspeed\n");
        }
    }
    if (!m_audioTrace || m_traceRows >= 20000) return;
    if (strcmp(event,"submit")==0 && !IsActive()) return;
    fprintf(m_audioTrace,"%lu,%llu,%s,%lu,%lu,%u,%u,%u,%u\n", GetTickCount(),
        static_cast<unsigned long long>(g_nCumulativeCycles), event, play, write,
        m_offset, static_cast<unsigned>(m_audio.size()), m_mea.Active()?1:0, g_bFullSpeed?1:0);
    if (++m_traceRows % 250 == 0 || m_traceRows == 20000) fflush(m_audioTrace);
}

const std::string& PorteParoleCard::GetSnapshotCardName()
{
    static const std::string name("Porte-Parole"); return name;
}

void PorteParoleCard::SaveSnapshot(YamlSaveHelper& yaml)
{
    Synchronize();
    YamlSaveHelper::Slot slot(yaml, GetSnapshotCardName(), m_slot, 1);
    YamlSaveHelper::Label state(yaml, "%s:\n", SS_YAML_KEY_STATE);
    yaml.SaveHexUint64("Last Cycle", m_lastCycle);
    yaml.Save("Clock Fraction: %.17g\n", m_clockFraction);
    auto v = m_mea.SaveState();
    yaml.SaveUint("Field Count", static_cast<UINT>(v.size()));
    for (size_t i = 0; i < v.size(); ++i) yaml.Save("S%03u: %.17g\n", static_cast<UINT>(i), v[i]);
}

bool PorteParoleCard::LoadSnapshot(YamlLoadHelper& yaml, UINT version)
{
    if (version != 1) ThrowErrorInvalidVersion(version);
    m_lastCycle = yaml.LoadUint64("Last Cycle");
    m_clockFraction = yaml.LoadDouble("Clock Fraction");
    if (!std::isfinite(m_clockFraction) || m_clockFraction < 0 || m_clockFraction >= 1)
        throw std::runtime_error("Porte-Parole invalid clock fraction");
    UINT count = yaml.LoadUint("Field Count");
    if (count != m_mea.SaveState().size()) throw std::runtime_error("Porte-Parole state length");
    std::vector<double> v;
    for (UINT i = 0; i < count; ++i)
    {
        char key[16]; sprintf_s(key, "S%03u", i); v.push_back(yaml.LoadDouble(key));
    }
    m_mea.LoadState(v); Destroy();
    return true;
}

bool PorteParoleCard::IsActive() const
{
    // 200 ms host buffer + analogue tail. Explicit user turbo remains available.
    return m_mea.Active() || g_nCumulativeCycles < m_preventTurboUntil;
}

bool PorteParoleActive()
{
    for (UINT slot = SLOT1; slot <= SLOT7; ++slot)
        if (GetCardMgr().QuerySlot(slot) == CT_PorteParole &&
            static_cast<PorteParoleCard&>(GetCardMgr().GetRef(slot)).IsActive()) return true;
    return false;
}
