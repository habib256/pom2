// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "Card.h"
#include "Mea8000.h"
#include "SoundCore.h"

class PorteParoleCard : public Card
{
public:
    explicit PorteParoleCard(UINT slot);
    virtual ~PorteParoleCard() { Destroy(); }
    void InitializeIO(LPBYTE) override;
    void Destroy() override;
    void Reset(bool powerCycle) override;
    void Update(ULONG) override;
    void SaveSnapshot(YamlSaveHelper&) override;
    bool LoadSnapshot(YamlLoadHelper&, UINT version) override;
    static const std::string& GetSnapshotCardName();
    static BYTE __stdcall IO(WORD, WORD addr, BYTE write, BYTE value, ULONG cycles);
    bool IsActive() const;
private:
    Mea8000 m_mea;
    VOICE m_voice;
    UINT64 m_lastCycle;
    double m_clockFraction = 0;
    uint32_t m_offset = UINT32_MAX;
    bool m_wasFullSpeed = false;
    UINT64 m_preventTurboUntil = 0;
    std::vector<int16_t> m_audio;
    FILE* m_audioTrace = nullptr;
    bool m_traceAttempted = false;
    unsigned m_traceRows = 0;
    void TraceAudio(const char* event, DWORD play = 0, DWORD write = 0);
    void Synchronize();
    void FlushAudio();
    static constexpr uint32_t BufferBytes = 19200; // 200 ms, mono 48 kHz / 16-bit
};

bool PorteParoleActive();
