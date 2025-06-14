#pragma once
#include <distingnt/api.h>
#include <faust/dsp/dsp.h>
#include <faust/gui/UI.h>
#include <faust/gui/meta.h>
#include <cstddef>
#include <cstdint>

/*────────────────   tiny manager the host can probe   ───────────────*/
struct MemoryMgr : public dsp_memory_manager
{
    enum Mode { Probe, Allocate };
    Mode    mode;
    size_t  total = 0;
    uint8_t* base = nullptr;

    explicit MemoryMgr(Mode m): mode(m) {}
    void begin(size_t) override { total = 0; }
    void end()   override {}
    void info(size_t size, size_t /*r*/, size_t w) override
    { if (mode==Probe && w==0) total += size; }
    void* allocate(size_t size) override
    { void* p = base + total; total += size; return p; }
    void destroy(void*) override {}
};

/* helper: convert local→global index and push via host */
inline void pushParam(_NT_algorithm* self, int localIdx, int16_t value)
{
    NT_setParameterFromUi(
        NT_algorithmIndex(self),
        localIdx + NT_parameterOffset(),
        value
    );
}
