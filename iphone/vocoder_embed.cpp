// Embed the Disting NT vocoder implementation in a portable translation unit.
//
// The algorithm itself is deliberately kept in vocoder/vocoder_algo.cpp so the
// iPhone build and the Disting plug-in continue to share the same filter bank,
// envelope follower, overload guard, and parameter behaviour.  These no-op NT
// services are the small part of the host ABI that the DSP translation unit
// needs when it is used outside the Disting firmware.

#include "../distingnt_api/include/distingnt/api.h"
#include "../distingnt_api/include/distingnt/serialisation.h"

#include <cstdint>
#include <cstring>

const _NT_globals NT_globals = {
    .sampleRate = 48000,
    .maxFramesPerStep = 24,
    .workBuffer = nullptr,
    .workBufferSizeBytes = 0,
    .streamSizeBytes = 0,
    .streamBufferSizeBytes = 0,
};

uint8_t NT_screen[128 * 64] = {};

inline void NT_drawText(int, int, const char *, int, _NT_textAlignment,
                        _NT_textSize) {}
inline void NT_drawShapeI(_NT_shape, int, int, int, int, int) {}
inline void NT_setParameterFromUi(int, int, int) {}
inline int32_t NT_algorithmIndex(const _NT_algorithm *) { return 0; }
inline uint32_t NT_parameterOffset(void) { return 0; }
inline uint32_t NT_getCpuCycleCount(void) { return 0; }

inline bool draw(_NT_algorithm *) { return false; }
inline uint32_t hasCustomUi(_NT_algorithm *) { return 0; }
inline void customUi(_NT_algorithm *, const _NT_uiData &) {}
inline void setupUi(_NT_algorithm *, _NT_float3 &) {}

inline void _NT_jsonStream::addMemberName(const char *) {}
inline void _NT_jsonStream::addNumber(int) {}
inline void _NT_jsonStream::addNumber(float) {}
inline bool _NT_jsonParse::numberOfObjectMembers(int &num) {
  num = 0;
  return true;
}
inline bool _NT_jsonParse::matchName(const char *) { return false; }
inline bool _NT_jsonParse::number(int &) { return false; }
inline bool _NT_jsonParse::skipMember(void) { return true; }

#include "../vocoder/vocoder_algo.cpp"
