/*
 * RINGS: a resonator engine for the Deluge, ported from Mutable Instruments Rings.
 *
 * Portions Copyright 2015 Emilie Gillet (Mutable Instruments Rings and stmlib, MIT licence: see rings.h).
 * Deluge port Copyright © 2026 thisisbribery.
 *
 * What was changed for the Deluge, to make one Rings voice cost about what one Deluge synth voice does:
 * - One Rings voice per Deluge voice, at 44.1 kHz, with the internal exciter. The models are set up as Rings sets each
 *   voice up at 2-voice polyphony: 4 strings for the sympathetic and chord models (Bryan Noll's chord table, which
 *   Rings ships with), 1 string for the inharmonic string. The modal resonator has 24 modes (Rings has 28 at 2 voices,
 *   12 at 4).
 * - The modes are Rings' own band passes (stmlib's TPT state variable filter), rewritten as the equivalent direct form
 *   biquads, which share their (x[n] - x[n-2]) numerator, and run four at a time on NEON. The lowest four keep their
 *   poles as small offsets from 2 and 1 so low notes stay in tune in single precision. The pickup position's gains are
 *   worked out per render, not per sample, and ramp across the render when they change.
 * - The four strings run side by side on NEON, their delay lines addressed in fixed point so no float has to come back
 *   to an integer register per sample. The sympathetic strings hear the struck string one sample late rather than a
 *   block late. The lowest string is about 21.5 Hz: Rings' sample rate conversion for lower strings is left out.
 * - Rings' limiter follows its peak eight samples at a time.
 * - The FM voice's sines are a polynomial rather than a table, and its phases are floats. Its envelope follower, which
 *   only ever hears the bow's white noise here, follows a render at a time towards what Rings' Follower reads from
 *   white noise.
 * - The strings' settings are worked out once a render (up to 64 samples) rather than every 24, and only again when
 *   what they come from changes. The four strings' damping steps once a render; only their delays are interpolated. A
 * new voice's strings start at their pitch rather than gliding from 220 Hz.
 * - The inharmonic string's delay runs in fixed point unless noise or the curved bridge move it (structure under 0.24
 *   or over about 0.81). Its all pass line follows the string line's write position.
 * - Rings' note filter, stereo outputs and reverb are left out. Mono is Rings' 2-voice mix: odd
 *   minus even modes, string minus pickup.
 * - Bow: filtered noise into the exciter path for as long as the voice plays. Drive: gain into the soft clipper.
 * - Delay lines are cleared lazily, only as far as the strings read, so a new note doesn't clear 40 KB.
 */

#include "dsp/drum_synth/rings.h"

#include <algorithm>
#include <arm_neon.h>
#include <array>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace deluge::dsp::drum_synth::rings {

namespace {

constexpr float kSampleRate = 44100.f;
constexpr int32_t kMaxRender = 64;
constexpr int32_t kBlock = 24; ///< Rings' control rate block
constexpr int32_t kMaxModes = 24;
constexpr int32_t kModeGroups = kMaxModes / 4;
constexpr int32_t kNumStrings = 4;
constexpr int32_t kLineBits = 11;
constexpr int32_t kLineSize = 1 << kLineBits; ///< 2048, as Rings
constexpr uint32_t kLineMask = kLineSize - 1;
constexpr int32_t kLineGuard = 4; ///< The first samples again at the end, so four taps read in one go
/// Each line's place in memory, a quarter of a cache way (2 KB) further on than a whole number of ways. The four
/// strings all write at the same index every sample: lines exactly 8 KB apart land in the same few sets of the A9's
/// 4-way L1 and evict each other on every sample (out to SDRAM), which four voices of sympathetic strings couldn't keep
/// up with.
constexpr int32_t kLineStride = kLineSize + 512;
static_assert((kLineStride * 4) % 8192 == 2048 && kLineStride >= kLineSize + kLineGuard);
constexpr int32_t kStretchSize = 1024;
constexpr int32_t kPluckSize = 256;
constexpr float kPi = 3.14159265358979f;

// Rings' tables (rings/resources.cc)
constexpr std::array<float, 257> kStiffness{{
    -6.250000000e-02f, -6.152343750e-02f, -6.054687500e-02f, -5.957031250e-02f, -5.859375000e-02f, -5.761718750e-02f,
    -5.664062500e-02f, -5.566406250e-02f, -5.468750000e-02f, -5.371093750e-02f, -5.273437500e-02f, -5.175781250e-02f,
    -5.078125000e-02f, -4.980468750e-02f, -4.882812500e-02f, -4.785156250e-02f, -4.687500000e-02f, -4.589843750e-02f,
    -4.492187500e-02f, -4.394531250e-02f, -4.296875000e-02f, -4.199218750e-02f, -4.101562500e-02f, -4.003906250e-02f,
    -3.906250000e-02f, -3.808593750e-02f, -3.710937500e-02f, -3.613281250e-02f, -3.515625000e-02f, -3.417968750e-02f,
    -3.320312500e-02f, -3.222656250e-02f, -3.125000000e-02f, -3.027343750e-02f, -2.929687500e-02f, -2.832031250e-02f,
    -2.734375000e-02f, -2.636718750e-02f, -2.539062500e-02f, -2.441406250e-02f, -2.343750000e-02f, -2.246093750e-02f,
    -2.148437500e-02f, -2.050781250e-02f, -1.953125000e-02f, -1.855468750e-02f, -1.757812500e-02f, -1.660156250e-02f,
    -1.562500000e-02f, -1.464843750e-02f, -1.367187500e-02f, -1.269531250e-02f, -1.171875000e-02f, -1.074218750e-02f,
    -9.765625000e-03f, -8.789062500e-03f, -7.812500000e-03f, -6.835937500e-03f, -5.859375000e-03f, -4.882812500e-03f,
    -3.906250000e-03f, -2.929687500e-03f, -1.953125000e-03f, -9.765625000e-04f, 0.000000000e+00f,  0.000000000e+00f,
    0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,
    0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  6.029410294e-05f,
    3.672617230e-04f,  6.835957809e-04f,  1.009582073e-03f,  1.345515115e-03f,  1.691698412e-03f,  2.048444725e-03f,
    2.416076364e-03f,  2.794925468e-03f,  3.185334315e-03f,  3.587655624e-03f,  4.002252878e-03f,  4.429500650e-03f,
    4.869784943e-03f,  5.323503537e-03f,  5.791066350e-03f,  6.272895808e-03f,  6.769427226e-03f,  7.281109202e-03f,
    7.808404022e-03f,  8.351788076e-03f,  8.911752293e-03f,  9.488802580e-03f,  1.008346028e-02f,  1.069626264e-02f,
    1.132776331e-02f,  1.197853283e-02f,  1.264915914e-02f,  1.334024813e-02f,  1.405242417e-02f,  1.478633069e-02f,
    1.554263074e-02f,  1.632200761e-02f,  1.712516545e-02f,  1.795282987e-02f,  1.880574864e-02f,  1.968469234e-02f,
    2.059045506e-02f,  2.152385512e-02f,  2.248573583e-02f,  2.347696619e-02f,  2.449844176e-02f,  2.555108540e-02f,
    2.663584813e-02f,  2.775370999e-02f,  2.890568094e-02f,  3.009280173e-02f,  3.131614488e-02f,  3.257681565e-02f,
    3.387595299e-02f,  3.521473064e-02f,  3.659435812e-02f,  3.801608189e-02f,  3.948118641e-02f,  4.099099536e-02f,
    4.254687278e-02f,  4.415022437e-02f,  4.580249868e-02f,  4.750518848e-02f,  4.925983210e-02f,  5.106801479e-02f,
    5.293137017e-02f,  5.485158172e-02f,  5.683038428e-02f,  5.886956562e-02f,  6.097096806e-02f,  6.313649016e-02f,
    6.536808837e-02f,  6.766777886e-02f,  7.003763933e-02f,  7.247981084e-02f,  7.499649981e-02f,  7.758997998e-02f,
    8.026259446e-02f,  8.301675786e-02f,  8.585495846e-02f,  8.877976048e-02f,  9.179380636e-02f,  9.489981918e-02f,
    9.810060511e-02f,  1.013990559e-01f,  1.047981517e-01f,  1.083009634e-01f,  1.119106556e-01f,  1.156304895e-01f,
    1.194638260e-01f,  1.234141283e-01f,  1.274849653e-01f,  1.316800149e-01f,  1.360030671e-01f,  1.404580277e-01f,
    1.450489216e-01f,  1.497798965e-01f,  1.546552266e-01f,  1.596793166e-01f,  1.648567056e-01f,  1.701920711e-01f,
    1.756902336e-01f,  1.813561603e-01f,  1.871949702e-01f,  1.932119385e-01f,  1.994125013e-01f,  2.058022605e-01f,
    2.123869891e-01f,  2.191726361e-01f,  2.261653322e-01f,  2.333713949e-01f,  2.407973346e-01f,  2.484498605e-01f,
    2.563358863e-01f,  2.644625367e-01f,  2.728371538e-01f,  2.814673039e-01f,  2.903607839e-01f,  2.995256288e-01f,
    3.089701187e-01f,  3.187027863e-01f,  3.287324247e-01f,  3.390680953e-01f,  3.497191360e-01f,  3.606951697e-01f,
    3.720061128e-01f,  3.836621843e-01f,  3.956739150e-01f,  4.080521572e-01f,  4.208080940e-01f,  4.339532500e-01f,
    4.474995013e-01f,  4.614590865e-01f,  4.758446177e-01f,  4.906690914e-01f,  5.059459012e-01f,  5.216888491e-01f,
    5.379121581e-01f,  5.546304856e-01f,  5.718589358e-01f,  5.896130741e-01f,  6.079089407e-01f,  6.267630651e-01f,
    6.461924814e-01f,  6.662147434e-01f,  6.868479405e-01f,  7.081107139e-01f,  7.300222738e-01f,  7.526024164e-01f,
    7.758715422e-01f,  7.998506739e-01f,  8.245614757e-01f,  8.500262730e-01f,  8.762680723e-01f,  9.033105820e-01f,
    9.311782340e-01f,  9.598962059e-01f,  9.894904431e-01f,  1.000000745e+00f,  1.000037649e+00f,  1.000262504e+00f,
    1.000964607e+00f,  1.002570034e+00f,  1.005639154e+00f,  1.010861180e+00f,  1.019043988e+00f,  1.031097087e+00f,
    1.048005353e+00f,  1.070791059e+00f,  1.100461817e+00f,  1.137942574e+00f,  1.183990632e+00f,  1.239094135e+00f,
    1.303356514e+00f,  1.376372085e+00f,  1.457101344e+00f,  1.543758274e+00f,  1.633725943e+00f,  1.723520185e+00f,
    1.808823654e+00f,  1.884612937e+00f,  1.945398753e+00f,  2.000000000e+00f,  2.000000000e+00f,
}};
constexpr std::array<float, 257> kFourDecades{{
    1.000000000e+00f, 1.036632928e+00f, 1.074607828e+00f, 1.113973860e+00f, 1.154781985e+00f, 1.197085030e+00f,
    1.240937761e+00f, 1.286396945e+00f, 1.333521432e+00f, 1.382372227e+00f, 1.433012570e+00f, 1.485508017e+00f,
    1.539926526e+00f, 1.596338544e+00f, 1.654817100e+00f, 1.715437896e+00f, 1.778279410e+00f, 1.843422992e+00f,
    1.910952975e+00f, 1.980956779e+00f, 2.053525026e+00f, 2.128751662e+00f, 2.206734069e+00f, 2.287573200e+00f,
    2.371373706e+00f, 2.458244069e+00f, 2.548296748e+00f, 2.641648320e+00f, 2.738419634e+00f, 2.838735965e+00f,
    2.942727176e+00f, 3.050527890e+00f, 3.162277660e+00f, 3.278121151e+00f, 3.398208329e+00f, 3.522694651e+00f,
    3.651741273e+00f, 3.785515249e+00f, 3.924189758e+00f, 4.067944321e+00f, 4.216965034e+00f, 4.371444813e+00f,
    4.531583638e+00f, 4.697588817e+00f, 4.869675252e+00f, 5.048065717e+00f, 5.232991147e+00f, 5.424690937e+00f,
    5.623413252e+00f, 5.829415347e+00f, 6.042963902e+00f, 6.264335367e+00f, 6.493816316e+00f, 6.731703824e+00f,
    6.978305849e+00f, 7.233941627e+00f, 7.498942093e+00f, 7.773650302e+00f, 8.058421878e+00f, 8.353625470e+00f,
    8.659643234e+00f, 8.976871324e+00f, 9.305720409e+00f, 9.646616199e+00f, 1.000000000e+01f, 1.036632928e+01f,
    1.074607828e+01f, 1.113973860e+01f, 1.154781985e+01f, 1.197085030e+01f, 1.240937761e+01f, 1.286396945e+01f,
    1.333521432e+01f, 1.382372227e+01f, 1.433012570e+01f, 1.485508017e+01f, 1.539926526e+01f, 1.596338544e+01f,
    1.654817100e+01f, 1.715437896e+01f, 1.778279410e+01f, 1.843422992e+01f, 1.910952975e+01f, 1.980956779e+01f,
    2.053525026e+01f, 2.128751662e+01f, 2.206734069e+01f, 2.287573200e+01f, 2.371373706e+01f, 2.458244069e+01f,
    2.548296748e+01f, 2.641648320e+01f, 2.738419634e+01f, 2.838735965e+01f, 2.942727176e+01f, 3.050527890e+01f,
    3.162277660e+01f, 3.278121151e+01f, 3.398208329e+01f, 3.522694651e+01f, 3.651741273e+01f, 3.785515249e+01f,
    3.924189758e+01f, 4.067944321e+01f, 4.216965034e+01f, 4.371444813e+01f, 4.531583638e+01f, 4.697588817e+01f,
    4.869675252e+01f, 5.048065717e+01f, 5.232991147e+01f, 5.424690937e+01f, 5.623413252e+01f, 5.829415347e+01f,
    6.042963902e+01f, 6.264335367e+01f, 6.493816316e+01f, 6.731703824e+01f, 6.978305849e+01f, 7.233941627e+01f,
    7.498942093e+01f, 7.773650302e+01f, 8.058421878e+01f, 8.353625470e+01f, 8.659643234e+01f, 8.976871324e+01f,
    9.305720409e+01f, 9.646616199e+01f, 1.000000000e+02f, 1.036632928e+02f, 1.074607828e+02f, 1.113973860e+02f,
    1.154781985e+02f, 1.197085030e+02f, 1.240937761e+02f, 1.286396945e+02f, 1.333521432e+02f, 1.382372227e+02f,
    1.433012570e+02f, 1.485508017e+02f, 1.539926526e+02f, 1.596338544e+02f, 1.654817100e+02f, 1.715437896e+02f,
    1.778279410e+02f, 1.843422992e+02f, 1.910952975e+02f, 1.980956779e+02f, 2.053525026e+02f, 2.128751662e+02f,
    2.206734069e+02f, 2.287573200e+02f, 2.371373706e+02f, 2.458244069e+02f, 2.548296748e+02f, 2.641648320e+02f,
    2.738419634e+02f, 2.838735965e+02f, 2.942727176e+02f, 3.050527890e+02f, 3.162277660e+02f, 3.278121151e+02f,
    3.398208329e+02f, 3.522694651e+02f, 3.651741273e+02f, 3.785515249e+02f, 3.924189758e+02f, 4.067944321e+02f,
    4.216965034e+02f, 4.371444813e+02f, 4.531583638e+02f, 4.697588817e+02f, 4.869675252e+02f, 5.048065717e+02f,
    5.232991147e+02f, 5.424690937e+02f, 5.623413252e+02f, 5.829415347e+02f, 6.042963902e+02f, 6.264335367e+02f,
    6.493816316e+02f, 6.731703824e+02f, 6.978305849e+02f, 7.233941627e+02f, 7.498942093e+02f, 7.773650302e+02f,
    8.058421878e+02f, 8.353625470e+02f, 8.659643234e+02f, 8.976871324e+02f, 9.305720409e+02f, 9.646616199e+02f,
    1.000000000e+03f, 1.036632928e+03f, 1.074607828e+03f, 1.113973860e+03f, 1.154781985e+03f, 1.197085030e+03f,
    1.240937761e+03f, 1.286396945e+03f, 1.333521432e+03f, 1.382372227e+03f, 1.433012570e+03f, 1.485508017e+03f,
    1.539926526e+03f, 1.596338544e+03f, 1.654817100e+03f, 1.715437896e+03f, 1.778279410e+03f, 1.843422992e+03f,
    1.910952975e+03f, 1.980956779e+03f, 2.053525026e+03f, 2.128751662e+03f, 2.206734069e+03f, 2.287573200e+03f,
    2.371373706e+03f, 2.458244069e+03f, 2.548296748e+03f, 2.641648320e+03f, 2.738419634e+03f, 2.838735965e+03f,
    2.942727176e+03f, 3.050527890e+03f, 3.162277660e+03f, 3.278121151e+03f, 3.398208329e+03f, 3.522694651e+03f,
    3.651741273e+03f, 3.785515249e+03f, 3.924189758e+03f, 4.067944321e+03f, 4.216965034e+03f, 4.371444813e+03f,
    4.531583638e+03f, 4.697588817e+03f, 4.869675252e+03f, 5.048065717e+03f, 5.232991147e+03f, 5.424690937e+03f,
    5.623413252e+03f, 5.829415347e+03f, 6.042963902e+03f, 6.264335367e+03f, 6.493816316e+03f, 6.731703824e+03f,
    6.978305849e+03f, 7.233941627e+03f, 7.498942093e+03f, 7.773650302e+03f, 8.058421878e+03f, 8.353625470e+03f,
    8.659643234e+03f, 8.976871324e+03f, 9.305720409e+03f, 9.646616199e+03f, 1.000000000e+04f,
}};
constexpr std::array<float, 257> kSvfShift{{
    2.500000000e-01f, 2.408119579e-01f, 2.316544611e-01f, 2.225575501e-01f, 2.135502761e-01f, 2.046602549e-01f,
    1.959132760e-01f, 1.873329789e-01f, 1.789406032e-01f, 1.707548172e-01f, 1.627916233e-01f, 1.550643347e-01f,
    1.475836177e-01f, 1.403575876e-01f, 1.333919506e-01f, 1.266901772e-01f, 1.202537001e-01f, 1.140821254e-01f,
    1.081734480e-01f, 1.025242668e-01f, 9.712999179e-02f, 9.198504051e-02f, 8.708302003e-02f, 8.241689360e-02f,
    7.797913038e-02f, 7.376183852e-02f, 6.975688172e-02f, 6.595598018e-02f, 6.235079694e-02f, 5.893301078e-02f,
    5.569437701e-02f, 5.262677742e-02f, 4.972226058e-02f, 4.697307381e-02f, 4.437168789e-02f, 4.191081545e-02f,
    3.958342416e-02f, 3.738274529e-02f, 3.530227864e-02f, 3.333579426e-02f, 3.147733169e-02f, 2.972119704e-02f,
    2.806195849e-02f, 2.649444041e-02f, 2.501371653e-02f, 2.361510230e-02f, 2.229414676e-02f, 2.104662398e-02f,
    1.986852431e-02f, 1.875604550e-02f, 1.770558386e-02f, 1.671372543e-02f, 1.577723728e-02f, 1.489305906e-02f,
    1.405829467e-02f, 1.327020425e-02f, 1.252619642e-02f, 1.182382076e-02f, 1.116076060e-02f, 1.053482614e-02f,
    9.943947824e-03f, 9.386169992e-03f, 8.859644866e-03f, 8.362626781e-03f, 7.893466717e-03f, 7.450607078e-03f,
    7.032576744e-03f, 6.637986365e-03f, 6.265523903e-03f, 5.913950392e-03f, 5.582095932e-03f, 5.268855886e-03f,
    4.973187279e-03f, 4.694105394e-03f, 4.430680542e-03f, 4.182035018e-03f, 3.947340207e-03f, 3.725813861e-03f,
    3.516717519e-03f, 3.319354065e-03f, 3.133065427e-03f, 2.957230396e-03f, 2.791262569e-03f, 2.634608406e-03f,
    2.486745394e-03f, 2.347180309e-03f, 2.215447582e-03f, 2.091107747e-03f, 1.973745986e-03f, 1.862970740e-03f,
    1.758412418e-03f, 1.659722154e-03f, 1.566570656e-03f, 1.478647104e-03f, 1.395658114e-03f, 1.317326764e-03f,
    1.243391669e-03f, 1.173606108e-03f, 1.107737206e-03f, 1.045565155e-03f, 9.868824789e-04f, 9.314933471e-04f,
    8.792129165e-04f, 8.298667176e-04f, 7.832900713e-04f, 7.393275405e-04f, 6.978324110e-04f, 6.586662024e-04f,
    6.216982059e-04f, 5.868050482e-04f, 5.538702800e-04f, 5.227839874e-04f, 4.934424252e-04f, 4.657476707e-04f,
    4.396072968e-04f, 4.149340639e-04f, 3.916456285e-04f, 3.696642688e-04f, 3.489166247e-04f, 3.293334538e-04f,
    3.108493994e-04f, 2.934027734e-04f, 2.769353496e-04f, 2.613921700e-04f, 2.467213608e-04f, 2.328739600e-04f,
    2.198037532e-04f, 2.074671201e-04f, 1.958228884e-04f, 1.848321967e-04f, 1.744583648e-04f, 1.646667709e-04f,
    1.554247368e-04f, 1.467014179e-04f, 1.384677010e-04f, 1.306961070e-04f, 1.233606989e-04f, 1.164369956e-04f,
    1.099018897e-04f, 1.037335710e-04f, 9.791145345e-05f, 9.241610615e-05f, 8.722918894e-05f, 8.233339098e-05f,
    7.771237301e-05f, 7.335071282e-05f, 6.923385378e-05f, 6.534805627e-05f, 6.168035179e-05f, 5.821849973e-05f,
    5.495094649e-05f, 5.186678690e-05f, 4.895572788e-05f, 4.620805405e-05f, 4.361459529e-05f, 4.116669618e-05f,
    3.885618709e-05f, 3.667535690e-05f, 3.461692730e-05f, 3.267402848e-05f, 3.084017618e-05f, 2.910925011e-05f,
    2.747547345e-05f, 2.593339362e-05f, 2.447786409e-05f, 2.310402715e-05f, 2.180729775e-05f, 2.058334818e-05f,
    1.942809362e-05f, 1.833767851e-05f, 1.730846370e-05f, 1.633701428e-05f, 1.542008813e-05f, 1.455462508e-05f,
    1.373773675e-05f, 1.296669683e-05f, 1.223893206e-05f, 1.155201359e-05f, 1.090364889e-05f, 1.029167410e-05f,
    9.714046817e-06f, 9.168839263e-06f, 8.654231857e-06f, 8.168507146e-06f, 7.710044069e-06f, 7.277312546e-06f,
    6.868868378e-06f, 6.483348419e-06f, 6.119466033e-06f, 5.776006796e-06f, 5.451824445e-06f, 5.145837051e-06f,
    4.857023409e-06f, 4.584419632e-06f, 4.327115929e-06f, 4.084253574e-06f, 3.855022035e-06f, 3.638656274e-06f,
    3.434434189e-06f, 3.241674210e-06f, 3.059733017e-06f, 2.888003398e-06f, 2.725912223e-06f, 2.572918525e-06f,
    2.428511705e-06f, 2.292209816e-06f, 2.163557965e-06f, 2.042126787e-06f, 1.927511018e-06f, 1.819328137e-06f,
    1.717217095e-06f, 1.620837105e-06f, 1.529866508e-06f, 1.444001699e-06f, 1.362956111e-06f, 1.286459263e-06f,
    1.214255852e-06f, 1.146104908e-06f, 1.081778982e-06f, 1.021063394e-06f, 9.637555088e-07f, 9.096640684e-07f,
    8.586085474e-07f, 8.104185525e-07f, 7.649332542e-07f, 7.220008496e-07f, 6.814780557e-07f, 6.432296314e-07f,
    6.071279262e-07f, 5.730524541e-07f, 5.408894912e-07f, 5.105316968e-07f, 4.818777544e-07f, 4.548320342e-07f,
    4.293042737e-07f, 4.052092763e-07f, 3.824666271e-07f, 3.610004248e-07f, 3.407390278e-07f, 3.216148157e-07f,
    3.035639631e-07f, 2.865262270e-07f, 2.704447456e-07f, 2.552658484e-07f, 2.409388772e-07f, 2.274160171e-07f,
    2.146521368e-07f, 2.026046381e-07f, 1.912333136e-07f, 1.805002124e-07f, 1.703695139e-07f, 1.608074078e-07f,
    1.517819816e-07f, 1.432631135e-07f, 1.352223728e-07f, 1.276329242e-07f, 1.204694386e-07f,
}};
constexpr std::array<float, 129> kFmQuantizer{{
    -1.200000000e+01f, -1.200000000e+01f, -1.200000000e+01f, -1.184000000e+01f, -1.184000000e+01f, -1.184000000e+01f,
    -1.111000000e+01f, -1.038000000e+01f, -9.650000000e+00f, -8.920000000e+00f, -8.190000000e+00f, -7.460000000e+00f,
    -6.730000000e+00f, -6.000000000e+00f, -6.000000000e+00f, -6.000000000e+00f, -5.545511612e+00f, -5.091023223e+00f,
    -4.636534835e+00f, -4.182046446e+00f, -4.182046446e+00f, -4.182046446e+00f, -3.659290641e+00f, -3.136534835e+00f,
    -2.613779029e+00f, -2.091023223e+00f, -1.568267417e+00f, -1.045511612e+00f, -5.227558058e-01f, 0.000000000e+00f,
    0.000000000e+00f,  0.000000000e+00f,  1.600000000e-01f,  1.600000000e-01f,  1.600000000e-01f,  8.900000000e-01f,
    1.620000000e+00f,  2.350000000e+00f,  3.080000000e+00f,  3.810000000e+00f,  4.540000000e+00f,  5.270000000e+00f,
    6.000000000e+00f,  6.000000000e+00f,  6.000000000e+00f,  6.454488388e+00f,  6.908976777e+00f,  7.363465165e+00f,
    7.817953554e+00f,  7.817953554e+00f,  7.817953554e+00f,  8.285529931e+00f,  8.753106309e+00f,  9.220682687e+00f,
    9.688259065e+00f,  9.688259065e+00f,  9.688259065e+00f,  1.026619430e+01f,  1.084412953e+01f,  1.142206477e+01f,
    1.200000000e+01f,  1.200000000e+01f,  1.200000000e+01f,  1.216000000e+01f,  1.216000000e+01f,  1.216000000e+01f,
    1.262977500e+01f,  1.309955001e+01f,  1.356932501e+01f,  1.403910002e+01f,  1.403910002e+01f,  1.403910002e+01f,
    1.490761987e+01f,  1.577613972e+01f,  1.664465957e+01f,  1.751317942e+01f,  1.751317942e+01f,  1.751317942e+01f,
    1.800000000e+01f,  1.800000000e+01f,  1.800000000e+01f,  1.850977500e+01f,  1.901955001e+01f,  1.901955001e+01f,
    1.901955001e+01f,  1.981795355e+01f,  1.981795355e+01f,  1.981795355e+01f,  2.066386428e+01f,  2.150977500e+01f,
    2.150977500e+01f,  2.150977500e+01f,  2.213233125e+01f,  2.275488750e+01f,  2.337744375e+01f,  2.400000000e+01f,
    2.400000000e+01f,  2.400000000e+01f,  2.450977500e+01f,  2.501955001e+01f,  2.501955001e+01f,  2.501955001e+01f,
    2.547403840e+01f,  2.592852679e+01f,  2.638301517e+01f,  2.683750356e+01f,  2.683750356e+01f,  2.683750356e+01f,
    2.735032035e+01f,  2.786313714e+01f,  2.786313714e+01f,  2.786313714e+01f,  2.839735285e+01f,  2.893156857e+01f,
    2.946578428e+01f,  3.000000000e+01f,  3.000000000e+01f,  3.000000000e+01f,  3.075000000e+01f,  3.150000000e+01f,
    3.225000000e+01f,  3.300000000e+01f,  3.375000000e+01f,  3.450000000e+01f,  3.525000000e+01f,  3.600000000e+01f,
    3.600000000e+01f,  3.600000000e+01f,  3.600000000e+01f,
}};

/// Rings' chord table at 4-voice polyphony, as it ships (BRYAN_CHORDS): two strings, the struck one first
constexpr float kChords2[11][2] = {
    {0.0f, -12.0f}, {0.0f, 2.0f},  {0.0f, 3.0f},  {0.0f, 4.0f},  {0.0f, 5.0f},    {0.0f, 7.0f},
    {0.0f, 9.0f},   {0.0f, 10.0f}, {0.0f, 11.0f}, {0.0f, 12.0f}, {-12.0f, 12.0f},
};

/// Rings' chord table at 2-voice polyphony, as it ships (BRYAN_CHORDS)
constexpr float kChords[11][4] = {
    {-12.0f, 0.0f, 0.01f, 12.0f},  // OCT
    {-12.0f, 6.99f, 7.0f, 12.0f},  // 5
    {-12.0f, 5.0f, 7.0f, 12.0f},   // sus4
    {-12.0f, 3.0f, 11.99f, 12.0f}, // m
    {-12.0f, 3.0f, 10.0f, 12.0f},  // m7
    {-12.0f, 3.0f, 10.0f, 14.0f},  // m9
    {-12.0f, 3.0f, 10.0f, 17.0f},  // m11
    {-12.0f, 2.0f, 9.0f, 16.0f},   // 69
    {-12.0f, 4.0f, 11.0f, 14.0f},  // M9
    {-12.0f, 4.0f, 7.0f, 11.0f},   // M7
    {-12.0f, 4.0f, 7.0f, 12.0f},   // M
};
constexpr int32_t kNumChords = 11;

/// Rings' model gains into its limiter
constexpr std::array<float, static_cast<size_t>(Model::NUM_MODELS)> kModelGain{1.4f, 1.0f, 1.4f, 0.7f, 1.0f};

// Block rate maths --------------------------------------------------------------------------------------------------

/// 2^r - 1 for r between -0.5 and 0.5, to about 1e-7 of the result: precise near 0, where decay coefficients live
inline float exp2Minus1(float r) {
	return r
	       * (0.69314718f
	          + r * (0.24022651f + r * (0.05550411f + r * (0.00961813f + r * (0.00133336f + r * 0.00015404f)))));
}

/// 2^x for block rate work, no libm: the nearest whole power times 2^r for the rest, so a coefficient just under 1 is
/// as accurate as a float can hold it
inline float exp2Fast(float x) {
	x = std::clamp(x, -126.f, 126.f);
	const auto k = static_cast<int32_t>(x + 127.5f) - 127; // nearest
	const float r = x - static_cast<float>(k);
	uint32_t bits = static_cast<uint32_t>(k + 127) << 23;
	float scale;
	std::memcpy(&scale, &bits, 4);
	return scale + scale * exp2Minus1(r);
}

inline float32x4_t exp2Fast(float32x4_t x) {
	x = vmaxq_f32(vminq_f32(x, vdupq_n_f32(126.f)), vdupq_n_f32(-126.f));
	const int32x4_t k = vsubq_s32(vcvtq_s32_f32(vaddq_f32(x, vdupq_n_f32(127.5f))), vdupq_n_s32(127));
	const float32x4_t r = vsubq_f32(x, vcvtq_f32_s32(k));
	float32x4_t p = vmlaq_f32(vdupq_n_f32(0.00133336f), r, vdupq_n_f32(0.00015404f));
	p = vmlaq_f32(vdupq_n_f32(0.00961813f), r, p);
	p = vmlaq_f32(vdupq_n_f32(0.05550411f), r, p);
	p = vmlaq_f32(vdupq_n_f32(0.24022651f), r, p);
	p = vmlaq_f32(vdupq_n_f32(0.69314718f), r, p);
	p = vmulq_f32(r, p); // 2^r - 1
	const float32x4_t scale = vreinterpretq_f32_s32(vshlq_n_s32(vaddq_s32(k, vdupq_n_s32(127)), 23));
	return vmlaq_f32(scale, scale, p);
}

/// log2(x) for x > 0, to about 1e-5, for block rate work
inline float log2Fast(float x) {
	uint32_t bits;
	std::memcpy(&bits, &x, 4);
	const auto e = static_cast<int32_t>((bits >> 23) & 0xFF) - 127;
	bits = (bits & 0x007FFFFFu) | 0x3F800000u;
	float m;
	std::memcpy(&m, &bits, 4); // 1 to 2
	// log2(m) = 2 atanh((m - 1) / (m + 1)) / ln 2
	const float t = (m - 1.f) / (m + 1.f);
	const float t2 = t * t;
	const float p = t * (2.8853901f + t2 * (0.9617967f + t2 * (0.5770780f + t2 * 0.4121653f)));
	return static_cast<float>(e) + p;
}
inline float semitonesToRatio(float s) {
	return exp2Fast(s * (1.f / 12.f));
}

/// 1 / x, two Newton steps from the estimate
inline float32x4_t reciprocal(float32x4_t x) {
	float32x4_t r = vrecpeq_f32(x);
	r = vmulq_f32(r, vrecpsq_f32(x, r));
	return vmulq_f32(r, vrecpsq_f32(x, r));
}
inline float reciprocal(float x) {
	return vgetq_lane_f32(reciprocal(vdupq_n_f32(x)), 0);
}

/// Linear interpolation into one of Rings' tables, index in table steps
template <size_t N>
inline float lookup(const std::array<float, N>& table, float index) {
	index = std::clamp(index, 0.f, static_cast<float>(N - 1) - 0.001f);
	const auto i = static_cast<int32_t>(index);
	const float f = index - static_cast<float>(i);
	return table[i] + (table[i + 1] - table[i]) * f;
}

/// stmlib's tangent approximations (OnePole::tan)
inline float tanDirty(float f) {
	constexpr float a = 3.736e-01f * kPi * kPi * kPi;
	return f * (kPi + a * f * f);
}
inline float tanFast(float f) {
	constexpr float a = 3.260e-01f * kPi * kPi * kPi;
	constexpr float b = 1.823e-01f * kPi * kPi * kPi * kPi * kPi;
	const float f2 = f * f;
	return f * (kPi + f2 * (a + b * f2));
}
inline float32x4_t tanAccurate(float32x4_t f) {
	constexpr float p3 = kPi * kPi * kPi;
	constexpr float p5 = p3 * kPi * kPi;
	constexpr float p7 = p5 * kPi * kPi;
	constexpr float p9 = p7 * kPi * kPi;
	constexpr float p11 = p9 * kPi * kPi;
	const float32x4_t f2 = vmulq_f32(f, f);
	float32x4_t p = vmlaq_f32(vdupq_n_f32(2.900525e-03f * p9), f2, vdupq_n_f32(9.5168091e-03f * p11));
	p = vmlaq_f32(vdupq_n_f32(5.33740603e-02f * p7), f2, p);
	p = vmlaq_f32(vdupq_n_f32(1.333923995e-01f * p5), f2, p);
	p = vmlaq_f32(vdupq_n_f32(3.333314036e-01f * p3), f2, p);
	p = vmlaq_f32(vdupq_n_f32(kPi), f2, p);
	return vmulq_f32(f, p);
}

/// stmlib's SoftLimit, clipped at +-3 where it turns back (SoftClip), four at a time
inline float32x4_t softClip(float32x4_t x) {
	x = vmaxq_f32(vminq_f32(x, vdupq_n_f32(3.f)), vdupq_n_f32(-3.f));
	const float32x4_t x2 = vmulq_f32(x, x);
	const float32x4_t num = vmulq_f32(x, vaddq_f32(vdupq_n_f32(27.f), x2));
	const float32x4_t den = vmlaq_f32(vdupq_n_f32(27.f), x2, vdupq_n_f32(9.f));
	return vmulq_f32(num, reciprocal(den));
}

/// sin(2 pi x) for x between -64 and 64: one conversion to wrap it (in the float registers, never the integer ones),
/// then sin(pi z) = z (1 - z^2) P(z^2) for z in -1 to 1, to 2e-7
[[gnu::always_inline]] inline float sine2Pi(float x) {
	const float t = x + 64.5f;
	const float z = 2.f * (t - static_cast<float>(static_cast<int32_t>(t))) - 1.f;
	const float u = z * z;
	const float u2 = u * u;
	const float g = (3.1415916f - 2.02609f * u) + u2 * ((0.523813377f - 0.0745209096f * u) + u2 * 0.00601023285f);
	return z * (1.f - u) * g;
}

/// max(x, 0) and min(x, 0), without a compare
[[gnu::always_inline]] inline float positivePart(float x) {
	return 0.5f * (x + std::fabs(x));
}

/// Rings' Squash()
inline float squash(float x) {
	if (x < 0.5f) {
		x *= 2.f;
		x *= x;
		x *= x;
		x *= x;
		x *= x;
		return x * 0.5f;
	}
	x = 2.f - 2.f * x;
	x *= x;
	x *= x;
	x *= x;
	x *= x;
	return 1.f - 0.5f * x;
}

uint32_t gSeed = 0x2545F491u;

/// What the inharmonic string's dispersion setting makes, at one end of a block (String::ProcessInternal<true>)
struct Dispersion {
	float value, stretchPoint, noiseAmount, bridgeCurving, acBlocking, apGain;
	bool noisy;  ///< Noise moves the delay (structure over about 0.81)
	bool curved; ///< The curved bridge moves the delay (structure under 0.24)
	explicit Dispersion(float d) {
		value = d;
		stretchPoint = (d <= 0.f) ? 0.f : d * (2.f - d) * 0.475f;
		const float noise = (d > 0.75f) ? 4.f * (d - 0.75f) : 0.f;
		noiseAmount = noise * noise * 0.025f;
		const float curving = (d < 0.f) ? -d : 0.f;
		acBlocking = curving;
		bridgeCurving = curving * curving * 0.01f;
		apGain = -0.618f * d / (0.15f + std::fabs(d));
		noisy = noiseAmount > 0.f;
		curved = bridgeCurving > 0.f || acBlocking > 0.f;
	}
};

} // namespace

// Voice -----------------------------------------------------------------------------------------------------------

class alignas(32) Voice {
public:
	// Modes, four to a vector: state, poles, numerator, output gains (signed: odd minus even)
	alignas(16) float y1[kMaxModes];
	alignas(16) float y2[kMaxModes];
	alignas(16) float c1[kMaxModes]; ///< 2 r cos(w)
	alignas(16) float a2[kMaxModes]; ///< r^2
	alignas(16) float b0[kMaxModes];
	alignas(16) float gain[kMaxModes];
	alignas(16) float e1[4]; ///< The lowest four: 2 - c1
	alignas(16) float e2[4]; ///< 1 - a2

	// Strings, one per lane
	alignas(16) float sFreq[4]; ///< Gliding frequency
	alignas(16) float sEff[4];  ///< Hermite delay at the end of the last block (Rings' delay * compensation - 1)
	alignas(16) float sComb[4]; ///< Pickup delay
	alignas(16) float sDamp[4]; ///< FIR damping coefficient
	alignas(16) float sH0[4];   ///< FIR taps from brightness
	alignas(16) float sH1[4];
	alignas(16) float firX1[4];
	alignas(16) float firX2[4];
	alignas(16) float iirS1[4];
	alignas(16) float iirS2[4];
	alignas(16) float lfoPhase[4]; ///< The sympathetic strings' pickup wobble, cycles
	// The strings' settings worked out last block, kept while nothing they come from changes
	alignas(16) float laneTarget[4]; ///< Frequency each string glides to
	alignas(16) float laneBright[4];
	alignas(16) float laneDamp[4];
	alignas(16) float laneGlideLog[4]; ///< log2(1 - glide coefficient) per 24-sample block
	alignas(16) float laneFreqKey[4];  ///< The gliding frequencies the rest was worked out at
	alignas(16) float laneDelay[4];
	alignas(16) float laneEff[4];
	alignas(16) float laneDampCoef[4];
	alignas(16) float laneH0[4];
	alignas(16) float laneH1[4];
	alignas(16) float laneG[4];
	alignas(16) float laneRg[4];
	alignas(16) float laneH[4];
	float laneKey[6]; ///< f0, structure, brightness, damping, position, strings the lane settings came from
	bool laneValid;
	bool derivedValid;
	bool stringsFresh; ///< Nothing played yet: the strings start at their settings rather than gliding to them

	// Excitation
	float excS1, excS2;
	float u1, u2; ///< Modal input history for the shared numerator
	float dcX, dcY;
	float sympPrev;
	// Plucker
	float plS1, plS2;
	float plPeriod, plGain, plG, plR, plH;
	int32_t plRemaining;
	uint32_t plWp;
	bool plActive;
	// Inharmonic string extras
	float dispNoise, curvedBridge, sdcX, sdcY;
	Dispersion dispersion;             ///< What the structure knob makes, now
	Dispersion prevDispersionSettings; ///< And at the end of the last render
	bool stretchClear;
	// FM
	float fmCarrier, fmModulator, fmPrev, fmAmpEnv, fmBrightEnv, fmGain, fmAmount;
	float fmPrevCarrierInc, fmPrevModInc, fmPrevBright, fmPrevFeedback;
	float flEnvelope, flCentroid; ///< Rings' Follower on the bow

	// Common
	float limiterPeak;
	float levelValue;
	float lastBow;
	float modalKey[5]; ///< frequency, structure, brightness, damping, position the modes were worked out for
	float lastPosition;
	int32_t numGroups;
	int32_t chord;
	int32_t holdoff;
	uint32_t rng;
	uint32_t wp;
	int32_t valid; ///< Samples back from wp that hold string data (since the voice was reset), up to a whole line
	Model model;
	bool pendingStrike;
	bool modalValid;
	bool silent;

	alignas(16) float scratch[kMaxRender * 4]; ///< The strings' per-lane outputs (kept off the stack, which is tight)
	alignas(16) float plComb[kPluckSize + 4];
	alignas(16) float stretch[kStretchSize];
	alignas(32) float line[kNumStrings][kLineStride];

	[[gnu::always_inline]] float noise() {
		rng = rng * 1664525u + 1013904223u;
		return static_cast<float>(static_cast<int32_t>(rng)) * (1.f / 2147483648.f);
	}
	[[gnu::always_inline]] float uniform() {
		rng = rng * 1664525u + 1013904223u;
		return static_cast<float>(rng >> 8) * (1.f / 16777216.f);
	}
};

static_assert(std::is_trivially_destructible_v<Voice>);

namespace {

void resetStrings(Voice& v) {
	for (int32_t l = 0; l < 4; l++) {
		v.sFreq[l] = 220.f / kSampleRate;
		v.sEff[l] = kSampleRate / 220.f - 1.f;
		v.sComb[l] = 0.f;
		v.sDamp[l] = 0.f;
		v.sH0[l] = 0.5f;
		v.sH1[l] = 0.25f;
		v.firX1[l] = v.firX2[l] = 0.f;
		v.iirS1[l] = v.iirS2[l] = 0.f;
	}
	// Each string's pickup wobble starts at its top, as Rings' LFOs do
	for (float& phase : v.lfoPhase) {
		phase = 0.f;
	}
	v.dcX = v.dcY = 0.f;
	v.sympPrev = 0.f;
	v.plS1 = v.plS2 = 0.f;
	v.plPeriod = 1.f;
	v.plGain = 0.f;
	v.plG = v.plR = v.plH = 0.f;
	v.plRemaining = 0;
	v.plWp = 0;
	v.plActive = false;
	std::memset(v.plComb, 0, sizeof(v.plComb));
	v.dispNoise = v.curvedBridge = 0.f;
	v.dispersion = Dispersion(0.f);
	v.prevDispersionSettings = Dispersion(0.f);
	v.sdcX = v.sdcY = 0.f;
	v.stretchClear = false;
	v.wp = 0;
	v.valid = 0;
	v.laneValid = false;
	v.derivedValid = false;
	v.stringsFresh = true;
	for (float& k : v.laneKey) {
		k = 0.f;
	}
}

void resetModes(Voice& v) {
	std::memset(v.y1, 0, sizeof(v.y1));
	std::memset(v.y2, 0, sizeof(v.y2));
	std::memset(v.gain, 0, sizeof(v.gain));
	v.u1 = v.u2 = 0.f;
	v.modalValid = false;
	v.lastPosition = -1.f;
	v.numGroups = 0;
}

void resetFm(Voice& v) {
	v.fmCarrier = v.fmModulator = v.fmPrev = 0.f;
	v.fmAmpEnv = v.fmBrightEnv = 0.f;
	v.fmGain = v.fmAmount = 0.f;
	v.fmPrevCarrierInc = v.fmPrevModInc = -1.f; // Jump to the first block's values
	v.fmPrevBright = v.fmPrevFeedback = 0.f;
	v.flEnvelope = v.flCentroid = 0.f;
}

/// Rings switches models by setting its resonators up again: so do we
void startModel(Voice& v, Model model) {
	v.model = model;
	v.excS1 = v.excS2 = 0.f;
	v.limiterPeak = 0.5f;
	resetModes(v);
	resetStrings(v);
	resetFm(v);
}

// Excitation ---------------------------------------------------------------------------------------------------------

/// Rings' excitation filter cutoff (normalised) at a brightness, for the internal exciter
float excitationCutoff(float f0, float brightness) {
	const float cutoff = brightness * (2.f - brightness);
	return std::min(f0 * semitonesToRatio((cutoff - 0.5f) * 96.f), 0.499f);
}

/// The bow's noise, before the excitation filter
float bowGain(float bow) {
	return bow * bow * 0.5f;
}

/// Excitation low pass (stmlib Svf, DIRTY tan, q 1.5) in place. Skipped while it has nothing in it.
void excitationFilter(Voice& v, float* x, int32_t n, float cutoff, bool hasInput) {
	if (!hasInput && std::fabs(v.excS1) + std::fabs(v.excS2) < 1e-9f) {
		v.excS1 = v.excS2 = 0.f;
		std::memset(x, 0, n * sizeof(float));
		return;
	}
	const float g = tanDirty(cutoff);
	const float r = 1.f / 1.5f;
	const float h = 1.f / (1.f + r * g + g * g);
	const float rg = r + g;
	float s1 = v.excS1;
	float s2 = v.excS2;
	for (int32_t i = 0; i < n; i++) {
		const float hp = (x[i] - rg * s1 - s2) * h;
		const float bp = g * hp + s1;
		s1 = g * hp + bp;
		const float lp = g * bp + s2;
		s2 = g * bp + lp;
		x[i] = lp;
	}
	v.excS1 = s1;
	v.excS2 = s2;
}

/// Fills x with the bow's noise, or zeros. Returns whether there is any.
bool bowNoise(Voice& v, float* x, int32_t n, float bow) {
	if (bow <= 0.f) {
		std::memset(x, 0, n * sizeof(float));
		return false;
	}
	const float g = bowGain(bow);
	for (int32_t i = 0; i < n; i++) {
		x[i] = v.noise() * g;
	}
	return true;
}

// Modal --------------------------------------------------------------------------------------------------------------

/// Rings' Resonator::ComputeFilters, into direct form biquads. The partials' frequencies and Qs run in order (each
/// leads on from the last); the filter coefficients are then worked out four at a time.
void computeModes(Voice& v, float frequency, float structure, float brightness, float damping) {
	alignas(16) float partials[kMaxModes];
	alignas(16) float qs[kMaxModes];
	float stiffness = lookup(kStiffness, structure * 256.f);
	float harmonic = frequency;
	float stretchFactor = 1.f;
	float q = 500.f * lookup(kFourDecades, damping * 256.f);
	float brightnessAttenuation = 1.f - structure;
	brightnessAttenuation *= brightnessAttenuation;
	brightnessAttenuation *= brightnessAttenuation;
	brightnessAttenuation *= brightnessAttenuation;
	const float b = brightness * (1.f - 0.2f * brightnessAttenuation);
	float qLoss = b * (2.f - b) * 0.85f + 0.15f;
	const float qLossDampingRate = structure * (2.f - structure) * 0.1f;
	int32_t numModes = 0;
	for (int32_t i = 0; i < kMaxModes; i++) {
		float partial = harmonic * stretchFactor;
		if (partial >= 0.49f) {
			partial = 0.49f;
		}
		else {
			numModes = i + 1;
		}
		partials[i] = partial;
		qs[i] = 1.f + partial * q; // Q
		stretchFactor += stiffness;
		stiffness *= (stiffness < 0.f) ? 0.93f : 0.98f;
		qLoss += qLossDampingRate * (1.f - qLoss);
		harmonic += frequency;
		q *= qLoss;
	}
	// stmlib Svf::set_f_q<FREQUENCY_FAST>(f, Q) as a band pass biquad:
	// H(z) = g h (1 - z^-2) / (1 - (2 - e1) z^-1 + (1 - e2) z^-2), r = 1 / Q, h = 1 / (1 + r g + g^2),
	// e1 = 2 g (r + 2 g) h, e2 = 2 g r h
	constexpr float kA = 3.260e-01f * kPi * kPi * kPi;
	constexpr float kB = 1.823e-01f * kPi * kPi * kPi * kPi * kPi;
	for (int32_t i = 0; i < kMaxModes; i += 4) {
		const float32x4_t f = vld1q_f32(partials + i);
		const float32x4_t f2 = vmulq_f32(f, f);
		const float32x4_t g = vmulq_f32(f, vmlaq_f32(vdupq_n_f32(kPi), f2, vmlaq_n_f32(vdupq_n_f32(kA), f2, kB)));
		const float32x4_t r = reciprocal(vld1q_f32(qs + i));
		const float32x4_t h = reciprocal(vmlaq_f32(vmlaq_f32(vdupq_n_f32(1.f), r, g), g, g));
		const float32x4_t gh2 = vmulq_f32(vaddq_f32(g, g), h);
		const float32x4_t e1 = vmulq_f32(gh2, vmlaq_n_f32(r, g, 2.f));
		const float32x4_t e2 = vmulq_f32(gh2, r);
		vst1q_f32(v.b0 + i, vmulq_n_f32(vmulq_f32(g, h), 0.125f)); // Rings scales the input by 1/8
		vst1q_f32(v.c1 + i, vsubq_f32(vdupq_n_f32(2.f), e1));
		vst1q_f32(v.a2 + i, vsubq_f32(vdupq_n_f32(1.f), e2));
		if (i == 0) {
			vst1q_f32(v.e1, e1);
			vst1q_f32(v.e2, e2);
		}
	}
	const int32_t groups = (numModes + 3) >> 2;
	// Modes that come back below Nyquist start from silence
	for (int32_t i = v.numGroups * 4; i < groups * 4; i++) {
		v.y1[i] = v.y2[i] = 0.f;
	}
	v.numGroups = groups;
	v.modalKey[4] = static_cast<float>(numModes); // Remembered for the gains
}

/// The pickup position's gain on each mode (stmlib CosineOscillator, approximate), odd minus even
void computeGains(float position, int32_t numModes, float* gains) {
	float f = position;
	float sign = 16.f;
	f -= 0.25f;
	if (f < 0.f) {
		f = -f;
	}
	else if (f > 0.5f) {
		f -= 0.5f;
	}
	else {
		sign = -16.f;
	}
	const float c = sign * f * (1.f - 2.f * f);
	float y1 = c * 0.25f;
	float y0 = 0.5f;
	for (int32_t i = 0; i < kMaxModes; i++) {
		const float a = y0 + 0.5f;
		const float next = c * y0 - y1;
		y1 = y0;
		y0 = next;
		gains[i] = (i < numModes) ? ((i & 1) ? -a : a) : 0.f;
	}
}

/// One sample of four modes: y = b0 dx - a2 y2 + c1 y1, the y1 term last so the recurrence waits on one multiply-add
[[gnu::always_inline]] inline float32x4_t modeStep(float32x4_t y1, float32x4_t y2, float32x4_t x, float32x4_t c,
                                                   float32x4_t a, float32x4_t b) {
	return vmlaq_f32(vmlsq_f32(vmulq_f32(b, x), a, y2), c, y1);
}
/// The same for the lowest four, with the poles as offsets: y = b0 dx + 2 y1 - y2 - e1 y1 + e2 y2
[[gnu::always_inline]] inline float32x4_t lowModeStep(float32x4_t y1, float32x4_t y2, float32x4_t x, float32x4_t e1,
                                                      float32x4_t e2, float32x4_t b) {
	const float32x4_t p = vsubq_f32(vmlaq_f32(vmulq_f32(b, x), e2, y2), y2);
	return vaddq_f32(vaddq_f32(p, y1), vmlsq_f32(y1, e1, y1));
}

/// Accumulate into the per-lane sums, or start them
template <bool kFirst>
[[gnu::always_inline]] inline float32x4_t accumulate(const float* acc, float32x4_t g, float32x4_t y) {
	if constexpr (kFirst) {
		return vmulq_f32(g, y);
	}
	else {
		return vmlaq_f32(vld1q_f32(acc), g, y);
	}
}

/// Modes 4k to 4k + 7 over n samples, two samples a pass so the states swap roles rather than move
template <bool kRamp, bool kFirst>
void modePairPass(Voice& v, int32_t k, const float* dx, float* acc, int32_t n, const float* gainStep) {
	const int32_t o = 4 * k;
	float32x4_t ya1 = vld1q_f32(v.y1 + o);
	float32x4_t ya2 = vld1q_f32(v.y2 + o);
	float32x4_t yb1 = vld1q_f32(v.y1 + o + 4);
	float32x4_t yb2 = vld1q_f32(v.y2 + o + 4);
	const float32x4_t ca = vld1q_f32(v.c1 + o);
	const float32x4_t cb = vld1q_f32(v.c1 + o + 4);
	const float32x4_t aa = vld1q_f32(v.a2 + o);
	const float32x4_t ab = vld1q_f32(v.a2 + o + 4);
	const float32x4_t ba = vld1q_f32(v.b0 + o);
	const float32x4_t bb = vld1q_f32(v.b0 + o + 4);
	float32x4_t ga = vld1q_f32(v.gain + o);
	float32x4_t gb = vld1q_f32(v.gain + o + 4);
	const float32x4_t gsa = kRamp ? vld1q_f32(gainStep + o) : vdupq_n_f32(0.f);
	const float32x4_t gsb = kRamp ? vld1q_f32(gainStep + o + 4) : vdupq_n_f32(0.f);
	int32_t i = 0;
	for (; i + 1 < n; i += 2) {
		const float32x4_t x0 = vld1q_dup_f32(dx + i);
		ya2 = modeStep(ya1, ya2, x0, ca, aa, ba);
		yb2 = modeStep(yb1, yb2, x0, cb, ab, bb);
		if constexpr (kRamp) {
			ga = vaddq_f32(ga, gsa);
			gb = vaddq_f32(gb, gsb);
		}
		vst1q_f32(acc + 4 * i, vmlaq_f32(accumulate<kFirst>(acc + 4 * i, ga, ya2), gb, yb2));
		const float32x4_t x1 = vld1q_dup_f32(dx + i + 1);
		ya1 = modeStep(ya2, ya1, x1, ca, aa, ba);
		yb1 = modeStep(yb2, yb1, x1, cb, ab, bb);
		if constexpr (kRamp) {
			ga = vaddq_f32(ga, gsa);
			gb = vaddq_f32(gb, gsb);
		}
		vst1q_f32(acc + 4 * i + 4, vmlaq_f32(accumulate<kFirst>(acc + 4 * i + 4, ga, ya1), gb, yb1));
	}
	if (i < n) {
		const float32x4_t x0 = vld1q_dup_f32(dx + i);
		const float32x4_t ta = modeStep(ya1, ya2, x0, ca, aa, ba);
		const float32x4_t tb = modeStep(yb1, yb2, x0, cb, ab, bb);
		ya2 = ya1;
		ya1 = ta;
		yb2 = yb1;
		yb1 = tb;
		if constexpr (kRamp) {
			ga = vaddq_f32(ga, gsa);
			gb = vaddq_f32(gb, gsb);
		}
		vst1q_f32(acc + 4 * i, vmlaq_f32(accumulate<kFirst>(acc + 4 * i, ga, ta), gb, tb));
	}
	vst1q_f32(v.y1 + o, ya1);
	vst1q_f32(v.y2 + o, ya2);
	vst1q_f32(v.y1 + o + 4, yb1);
	vst1q_f32(v.y2 + o + 4, yb2);
	vst1q_f32(v.gain + o, ga);
	vst1q_f32(v.gain + o + 4, gb);
}

/// Modes 4k to 4k + 3 over n samples
template <bool kRamp, bool kFirst>
void modeSinglePass(Voice& v, int32_t k, const float* dx, float* acc, int32_t n, const float* gainStep) {
	const int32_t o = 4 * k;
	float32x4_t ya1 = vld1q_f32(v.y1 + o);
	float32x4_t ya2 = vld1q_f32(v.y2 + o);
	const float32x4_t ca = vld1q_f32(v.c1 + o);
	const float32x4_t aa = vld1q_f32(v.a2 + o);
	const float32x4_t ba = vld1q_f32(v.b0 + o);
	float32x4_t ga = vld1q_f32(v.gain + o);
	const float32x4_t gsa = kRamp ? vld1q_f32(gainStep + o) : vdupq_n_f32(0.f);
	int32_t i = 0;
	for (; i + 1 < n; i += 2) {
		ya2 = modeStep(ya1, ya2, vld1q_dup_f32(dx + i), ca, aa, ba);
		if constexpr (kRamp) {
			ga = vaddq_f32(ga, gsa);
		}
		vst1q_f32(acc + 4 * i, accumulate<kFirst>(acc + 4 * i, ga, ya2));
		ya1 = modeStep(ya2, ya1, vld1q_dup_f32(dx + i + 1), ca, aa, ba);
		if constexpr (kRamp) {
			ga = vaddq_f32(ga, gsa);
		}
		vst1q_f32(acc + 4 * i + 4, accumulate<kFirst>(acc + 4 * i + 4, ga, ya1));
	}
	if (i < n) {
		const float32x4_t ta = modeStep(ya1, ya2, vld1q_dup_f32(dx + i), ca, aa, ba);
		ya2 = ya1;
		ya1 = ta;
		if constexpr (kRamp) {
			ga = vaddq_f32(ga, gsa);
		}
		vst1q_f32(acc + 4 * i, accumulate<kFirst>(acc + 4 * i, ga, ta));
	}
	vst1q_f32(v.y1 + o, ya1);
	vst1q_f32(v.y2 + o, ya2);
	vst1q_f32(v.gain + o, ga);
}

/// The resonator: dx is the input's x[n] - x[n-2], out gets the mode sum. Two samples a pass, so the states swap roles
/// rather than move.
template <bool kRamp>
void modalCore(Voice& v, const float* dx, float* out, int32_t n, const float* gainStep) {
	// Per-lane sums, on the stack (internal RAM) rather than in the voice (SDRAM)
	alignas(16) float acc[kMaxRender * 4];
	const int32_t groups = v.numGroups;
	if (groups == 0) {
		std::memset(out, 0, n * sizeof(float));
		return;
	}
	// The lowest four. Above about 130 Hz the plain form is in tune to a fraction of a cent: they join the rest.
	const bool precise = v.modalKey[0] < 0.003f;
	if (precise) {
		float32x4_t y1 = vld1q_f32(v.y1);
		float32x4_t y2 = vld1q_f32(v.y2);
		const float32x4_t e1 = vld1q_f32(v.e1);
		const float32x4_t e2 = vld1q_f32(v.e2);
		const float32x4_t b0 = vld1q_f32(v.b0);
		float32x4_t g = vld1q_f32(v.gain);
		const float32x4_t gs = kRamp ? vld1q_f32(gainStep) : vdupq_n_f32(0.f);
		int32_t i = 0;
		for (; i + 1 < n; i += 2) {
			y2 = lowModeStep(y1, y2, vld1q_dup_f32(dx + i), e1, e2, b0);
			if constexpr (kRamp) {
				g = vaddq_f32(g, gs);
			}
			vst1q_f32(acc + 4 * i, vmulq_f32(g, y2));
			y1 = lowModeStep(y2, y1, vld1q_dup_f32(dx + i + 1), e1, e2, b0);
			if constexpr (kRamp) {
				g = vaddq_f32(g, gs);
			}
			vst1q_f32(acc + 4 * i + 4, vmulq_f32(g, y1));
		}
		if (i < n) {
			const float32x4_t t = lowModeStep(y1, y2, vld1q_dup_f32(dx + i), e1, e2, b0);
			y2 = y1;
			y1 = t;
			if constexpr (kRamp) {
				g = vaddq_f32(g, gs);
			}
			vst1q_f32(acc + 4 * i, vmulq_f32(g, t));
		}
		vst1q_f32(v.y1, y1);
		vst1q_f32(v.y2, y2);
		vst1q_f32(v.gain, g);
	}
	// The rest, two groups a pass. Without the precise four, the first pass writes the sums rather than adding.
	int32_t k = precise ? 1 : 0;
	if (!precise && groups >= 2) {
		modePairPass<kRamp, true>(v, k, dx, acc, n, gainStep);
		k += 2;
	}
	for (; k + 1 < groups; k += 2) {
		modePairPass<kRamp, false>(v, k, dx, acc, n, gainStep);
	}
	if (k < groups) {
		if (k == 0) {
			modeSinglePass<kRamp, true>(v, k, dx, acc, n, gainStep);
		}
		else {
			modeSinglePass<kRamp, false>(v, k, dx, acc, n, gainStep);
		}
	}
	// Sum each sample's four lanes, four samples at a time
	const int32_t padded = (n + 3) & ~3;
	for (int32_t i = n; i < padded; i++) {
		vst1q_f32(acc + 4 * i, vdupq_n_f32(0.f));
	}
	for (int32_t i = 0; i < padded; i += 4) {
		const float32x4x4_t a = vld4q_f32(acc + 4 * i);
		vst1q_f32(out + i, vaddq_f32(vaddq_f32(a.val[0], a.val[1]), vaddq_f32(a.val[2], a.val[3])));
	}
}

[[gnu::noinline]] void renderModal(Voice& v, const Settings& s, float f0, bool strike, float* out, int32_t n) {
	alignas(16) float x[kMaxRender + 8];
	alignas(16) float dx[kMaxRender + 8];
	const float fc = excitationCutoff(f0, s.brightness);
	bool hasInput = bowNoise(v, x, n, s.bow);
	if (strike) {
		// Rings' internal exciter: a pulse into the excitation filter
		x[0] += 0.25f * semitonesToRatio(fc * fc * 24.f) / fc;
		hasInput = true;
	}
	excitationFilter(v, x, n, fc, hasInput);
	float u1 = v.u1;
	float u2 = v.u2;
	for (int32_t i = 0; i < n; i++) {
		dx[i] = x[i] - u2;
		u2 = u1;
		u1 = x[i];
	}
	v.u1 = u1;
	v.u2 = u2;

	const float brightness = s.brightness * s.brightness; // Rings squares it for the resonator
	if (!v.modalValid || f0 != v.modalKey[0] || s.structure != v.modalKey[1] || brightness != v.modalKey[2]
	    || s.damping != v.modalKey[3]) {
		const int32_t oldModes = static_cast<int32_t>(v.modalKey[4]);
		computeModes(v, f0, s.structure, brightness, s.damping);
		v.modalKey[0] = f0;
		v.modalKey[1] = s.structure;
		v.modalKey[2] = brightness;
		v.modalKey[3] = s.damping;
		if (!v.modalValid || static_cast<int32_t>(v.modalKey[4]) != oldModes) {
			v.lastPosition = -1.f; // The gains need redoing for the modes there are now
		}
		v.modalValid = true;
	}
	const int32_t numModes = static_cast<int32_t>(v.modalKey[4]);
	if (s.position != v.lastPosition) {
		alignas(16) float target[kMaxModes];
		computeGains(s.position, numModes, target);
		if (v.lastPosition < 0.f) {
			std::memcpy(v.gain, target, sizeof(target));
			v.lastPosition = s.position;
			modalCore<false>(v, dx, out, n, nullptr);
		}
		else {
			alignas(16) float step[kMaxModes];
			const float k = 1.f / static_cast<float>(n);
			for (int32_t i = 0; i < kMaxModes; i++) {
				step[i] = (target[i] - v.gain[i]) * k;
			}
			v.lastPosition = s.position;
			modalCore<true>(v, dx, out, n, step);
			std::memcpy(v.gain, target, sizeof(target));
		}
	}
	else {
		modalCore<false>(v, dx, out, n, nullptr);
	}
}

// Strings ------------------------------------------------------------------------------------------------------------

[[gnu::always_inline]] inline void writeLine(float* line, uint32_t i, float x) {
	line[i] = x;
	if (i < kLineGuard) {
		line[i + kLineSize] = x;
	}
}

/// The input every string voice shares (Rings' RenderStringVoice up to the strings): bow, gain, excitation filter,
/// the plucker's noise burst, DC blocker
void stringInput(Voice& v, const Settings& s, float f0, bool strike, int32_t numStrings, float* x, int32_t n) {
	const float fc = excitationCutoff(f0, s.brightness);
	bool hasInput = bowNoise(v, x, n, s.bow);
	if (hasInput) {
		const float g = 1.f / std::sqrt(static_cast<float>(numStrings) * 2.f);
		for (int32_t i = 0; i < n; i++) {
			x[i] *= g;
		}
	}
	excitationFilter(v, x, n, fc, hasInput);

	// Plucker: a noise burst through a comb, then a low pass
	if (strike) {
		const float ratio = s.position * 0.9f + 0.05f;
		float period = 1.f / f0 * ratio;
		v.plRemaining = static_cast<int32_t>(period);
		while (period >= 255.f) {
			period *= 0.5f;
		}
		v.plPeriod = period;
		v.plGain = (1.f - s.position) * 0.8f;
		v.plG = tanDirty(std::min(fc * 8.f, 0.499f));
		v.plR = 1.f;
		v.plH = 1.f / (1.f + v.plR * v.plG + v.plG * v.plG);
		v.plActive = true;
	}
	if (v.plActive) {
		const auto di = static_cast<uint32_t>(v.plPeriod);
		const float df = v.plPeriod - static_cast<float>(di);
		const float gain = v.plGain;
		const float g = v.plG;
		const float rg = v.plR + g;
		const float h = v.plH;
		float s1 = v.plS1;
		float s2 = v.plS2;
		uint32_t wp = v.plWp;
		float peak = 0.f;
		for (int32_t i = 0; i < n; i++) {
			float in = 0.f;
			if (v.plRemaining > 0) {
				in = 2.f * v.uniform() - 1.f;
				v.plRemaining--;
			}
			const float a = v.plComb[(wp + di) & (kPluckSize - 1)];
			const float b = v.plComb[(wp + di + 1) & (kPluckSize - 1)];
			const float y = in + gain * (a + (b - a) * df);
			v.plComb[wp] = y;
			wp = (wp - 1) & (kPluckSize - 1);
			const float hp = (y - rg * s1 - s2) * h;
			const float bp = g * hp + s1;
			s1 = g * hp + bp;
			const float lp = g * bp + s2;
			s2 = g * bp + lp;
			x[i] += lp;
			peak = std::max(peak, std::fabs(lp));
		}
		v.plS1 = s1;
		v.plS2 = s2;
		v.plWp = wp;
		if (v.plRemaining == 0 && peak < 1e-7f) {
			v.plActive = false;
			std::memset(v.plComb, 0, sizeof(v.plComb));
			v.plS1 = v.plS2 = 0.f;
		}
		hasInput = true;
	}

	// DC blocker, pole 1 - 10 / sr
	if (hasInput || std::fabs(v.dcY) > 1e-9f || std::fabs(v.dcX) > 1e-9f) {
		constexpr float kPole = 1.f - 10.f / kSampleRate;
		float dx = v.dcX;
		float dy = v.dcY;
		for (int32_t i = 0; i < n; i++) {
			const float old = dx;
			dx = x[i];
			x[i] = dy = dy * kPole + dx - old;
		}
		v.dcX = dx;
		v.dcY = dy;
	}
	else {
		v.dcX = v.dcY = 0.f;
	}
}

/// The strings' per-lane settings for one block (Rings' Part::RenderStringVoice and String::ProcessInternal setup)
struct LaneSetup {
	float32x4_t effStart, effEnd; ///< Hermite read delay across the block
	float32x4_t combStart, combEnd;
	float32x4_t dampStart, dampEnd;
	float32x4_t h0Start, h0End, h1Start, h1End;
	float32x4_t g, rg, h; ///< Damping low pass
};

/// Works out the strings' frequencies (two or four), as semitones above the played note
void sympatheticOffsets(Voice& v, const Settings& s, float* offsets, int32_t numStrings) {
	if (v.model == Model::CHORD) {
		// Structure picks the chord, with Rings' hysteresis
		const float c = s.structure * static_cast<float>(kNumChords - 1);
		const float hysteresis = (c - static_cast<float>(v.chord) > 0.f) ? -0.1f : 0.1f;
		v.chord = std::clamp<int32_t>(static_cast<int32_t>(c + hysteresis + 0.5f), 0, kNumChords - 1);
		for (int32_t i = 0; i < 4; i++) {
			offsets[i] = (numStrings == 2) ? kChords2[v.chord][std::min<int32_t>(i, 1)] : kChords[v.chord][i];
		}
		return;
	}
	// Rings' ComputeSympatheticStringsNotes with the tonic at the played note
	constexpr float kNotes[9] = {0.f, -12.f, -7.01955f, 0.f, 7.01955f, 12.f, 19.01955f, 24.f, 24.f};
	float parameter = s.structure;
	// Two strings: the struck one and one sympathetic, none detuned. Four: three, then the first detuned.
	const int32_t tuned = (numStrings == 2) ? 2 : 3;
	for (int32_t i = 0; i < tuned; i++) {
		float note = 3.f;
		if (i != 0) {
			note = parameter * 7.f;
			parameter += (1.f - parameter) * 0.2f;
		}
		const auto integral = std::clamp<int32_t>(static_cast<int32_t>(note), 0, 7);
		const float fractional = squash(note - static_cast<float>(integral));
		offsets[i] = kNotes[integral] + (kNotes[integral + 1] - kNotes[integral]) * fractional;
	}
	if (numStrings == 2) {
		offsets[2] = offsets[3] = offsets[1];
	}
	else {
		offsets[3] = offsets[0] + 0.013f;
	}
}

/// One block of the strings' settings (Rings' Part::RenderStringVoice and String::ProcessInternal setup). Only the
/// pickup's wobble and the glide move every block: the rest is worked out again only when what it comes from changes.
LaneSetup setupLanes(Voice& v, const Settings& s, float f0, int32_t numStrings, int32_t n) {
	// What each string is tuned to and how it rings
	if (!v.laneValid || v.laneKey[0] != f0 || v.laneKey[1] != s.structure || v.laneKey[2] != s.brightness
	    || v.laneKey[3] != s.damping || v.laneKey[5] != static_cast<float>(numStrings)) {
		float offsets[4] = {0.f, 0.f, 0.f, 0.f};
		if (numStrings > 1) {
			sympatheticOffsets(v, s, offsets, numStrings);
		}
		for (int32_t l = 0; l < 4; l++) {
			float b = s.brightness;
			float d = s.damping;
			float glideLog = -126.f; // Lands at once
			if (l > 0) {
				// The sympathetic strings
				b *= (2.f - b);
				b *= (2.f - b);
				d = 0.7f + s.damping * 0.27f;
				// glide = 2^(3 (b - 1)) a block; what's left after one is 1 - glide
				glideLog = log2Fast(1.f - semitonesToRatio((b - 1.f) * 36.f));
			}
			const int32_t lane = std::min(l, numStrings - 1);
			const float stringIndex = static_cast<float>(lane) / static_cast<float>(numStrings);
			v.laneTarget[l] = f0 * semitonesToRatio(offsets[lane]);
			v.laneBright[l] = b;
			v.laneDamp[l] = d + stringIndex * (0.95f - d);
			v.laneGlideLog[l] = glideLog;
		}
		v.laneKey[0] = f0;
		v.laneKey[1] = s.structure;
		v.laneKey[2] = s.brightness;
		v.laneKey[3] = s.damping;
		v.laneKey[5] = static_cast<float>(numStrings);
		v.laneValid = true;
		v.derivedValid = false;
	}

	// The LFOs (Rings': 0.5, 0.35, 0.211 and 0.171 Hz for a voice's strings) wobble the sympathetic strings' pickups
	constexpr float kLfoInc[4] = {0.5f / kSampleRate, 0.35f / kSampleRate, 0.211f / kSampleRate, 0.171f / kSampleRate};
	alignas(16) float lfoValues[4];
	for (int32_t l = 0; l < 4; l++) {
		lfoValues[l] = 0.5f + 0.5f * sine2Pi(v.lfoPhase[l] + 0.25f);
		float phase = v.lfoPhase[l] + kLfoInc[l] * static_cast<float>(n);
		v.lfoPhase[l] = phase - static_cast<float>(static_cast<int32_t>(phase));
	}
	const float32x4_t lfo = vld1q_f32(lfoValues);
	const float amount = (numStrings > 1) ? (0.5f - std::fabs(0.5f - s.position)) * 0.9f : 0.f;
	float32x4_t position = vmlaq_n_f32(vdupq_n_f32(s.position), lfo, amount);
	position = vsetq_lane_f32(s.position, position, 0);
	const float32x4_t clampedPosition =
	    vmlsq_f32(vdupq_n_f32(0.5f), vdupq_n_f32(0.98f), vabsq_f32(vsubq_f32(position, vdupq_n_f32(0.5f))));

	// Glide (String::set_frequency(f, coefficient)), as Rings would over this many of its 24-sample blocks
	float32x4_t freq = vld1q_f32(v.sFreq);
	const float32x4_t glide =
	    vsubq_f32(vdupq_n_f32(1.f), exp2Fast(vmulq_n_f32(vld1q_f32(v.laneGlideLog), static_cast<float>(n) / kBlock)));
	freq = v.stringsFresh ? vld1q_f32(v.laneTarget) : vmlaq_f32(freq, glide, vsubq_f32(vld1q_f32(v.laneTarget), freq));
	vst1q_f32(v.sFreq, freq);

	if (!v.derivedValid || std::memcmp(v.sFreq, v.laneFreqKey, sizeof(v.laneFreqKey)) != 0) {
		std::memcpy(v.laneFreqKey, v.sFreq, sizeof(v.laneFreqKey));
		v.derivedValid = true;
		// String::ProcessInternal's setup, four strings at a time
		const float32x4_t delay =
		    vminq_f32(vmaxq_f32(reciprocal(freq), vdupq_n_f32(4.f)), vdupq_n_f32(kLineSize - 4.f));
		const float32x4_t damping = vld1q_f32(v.laneDamp);
		const float32x4_t brightness0 = vld1q_f32(v.laneBright);
		const float32x4_t lfDamping = vmulq_f32(damping, vsubq_f32(vdupq_n_f32(2.f), damping));
		// rt60 = 0.07 * 2^(lfDamping * 8) * sr; -120 * delay / rt60 semitones, at most -127
		const float32x4_t rt60 =
		    vmulq_f32(vdupq_n_f32(0.07f * kSampleRate), exp2Fast(vmulq_f32(lfDamping, vdupq_n_f32(8.f))));
		const float32x4_t base =
		    vmaxq_f32(vmulq_f32(vmulq_f32(vdupq_n_f32(-120.f), delay), reciprocal(rt60)), vdupq_n_f32(-127.f));
		float32x4_t dampingCoefficient = exp2Fast(vmulq_f32(base, vdupq_n_f32(1.f / 12.f)));
		float32x4_t brightness = vmulq_f32(brightness0, brightness0);
		// damping cutoff = min(24 + damping^2 * 48 + brightness^2 * 24, 84) semitones
		float32x4_t dampingCutoff =
		    vmlaq_f32(vmlaq_f32(vdupq_n_f32(24.f), vmulq_f32(damping, damping), vdupq_n_f32(48.f)), brightness,
		              vdupq_n_f32(24.f));
		dampingCutoff = vminq_f32(dampingCutoff, vdupq_n_f32(84.f));
		float32x4_t dampingF = vminq_f32(vmulq_f32(freq, exp2Fast(vmulq_f32(dampingCutoff, vdupq_n_f32(1.f / 12.f)))),
		                                 vdupq_n_f32(0.499f));
		// Crossfade to infinite decay from damping 0.95
		const float32x4_t toInfinite =
		    vmulq_f32(vmaxq_f32(vsubq_f32(damping, vdupq_n_f32(0.95f)), vdupq_n_f32(0.f)), vdupq_n_f32(20.f));
		dampingCoefficient = vmlaq_f32(dampingCoefficient, toInfinite, vsubq_f32(vdupq_n_f32(1.f), dampingCoefficient));
		brightness = vmlaq_f32(brightness, toInfinite, vsubq_f32(vdupq_n_f32(1.f), brightness));
		dampingF = vmlaq_f32(dampingF, toInfinite, vsubq_f32(vdupq_n_f32(0.4999f), dampingF));
		dampingCutoff = vmlaq_f32(dampingCutoff, toInfinite, vsubq_f32(vdupq_n_f32(128.f), dampingCutoff));

		// Damping low pass, stmlib Svf::set_f_q<FREQUENCY_ACCURATE>(f, 0.5)
		const float32x4_t g = tanAccurate(dampingF);
		const float32x4_t r = vdupq_n_f32(2.f);
		vst1q_f32(v.laneG, g);
		vst1q_f32(v.laneRg, vaddq_f32(r, g));
		vst1q_f32(v.laneH, reciprocal(vmlaq_f32(vmlaq_f32(vdupq_n_f32(1.f), r, g), g, g)));

		// The delay the damping filter adds, taken off the string
		alignas(16) float cutoffs[4];
		alignas(16) float compensation[4];
		vst1q_f32(cutoffs, dampingCutoff);
		for (int32_t l = 0; l < 4; l++) {
			compensation[l] = 1.f - lookup(kSvfShift, cutoffs[l]);
		}
		vst1q_f32(v.laneDelay, delay);
		vst1q_f32(v.laneEff, vmlaq_f32(vdupq_n_f32(-1.f), delay, vld1q_f32(compensation)));
		vst1q_f32(v.laneDampCoef, dampingCoefficient);
		vst1q_f32(v.laneH0, vmulq_f32(vaddq_f32(vdupq_n_f32(1.f), brightness), vdupq_n_f32(0.5f)));
		vst1q_f32(v.laneH1, vmulq_f32(vsubq_f32(vdupq_n_f32(1.f), brightness), vdupq_n_f32(0.25f)));
	}

	// Rings interpolates delay, pickup position and compensation across the block: we interpolate what they make
	LaneSetup setup;
	const float32x4_t effTarget = vld1q_f32(v.laneEff);
	const float32x4_t combTarget = vmaxq_f32(vmulq_f32(vld1q_f32(v.laneDelay), clampedPosition), vdupq_n_f32(1.f));
	setup.effStart = vld1q_f32(v.sEff);
	setup.effEnd = effTarget;
	setup.combStart = vld1q_f32(v.sComb);
	setup.combEnd = combTarget;
	vst1q_f32(v.sEff, effTarget);
	vst1q_f32(v.sComb, combTarget);
	setup.dampStart = vld1q_f32(v.sDamp);
	setup.dampEnd = vld1q_f32(v.laneDampCoef);
	vst1q_f32(v.sDamp, setup.dampEnd);
	setup.h0Start = vld1q_f32(v.sH0);
	setup.h0End = vld1q_f32(v.laneH0);
	setup.h1Start = vld1q_f32(v.sH1);
	setup.h1End = vld1q_f32(v.laneH1);
	vst1q_f32(v.sH0, setup.h0End);
	vst1q_f32(v.sH1, setup.h1End);
	setup.g = vld1q_f32(v.laneG);
	setup.rg = vld1q_f32(v.laneRg);
	setup.h = vld1q_f32(v.laneH);
	if (v.stringsFresh) {
		v.stringsFresh = false;
		setup.effStart = setup.effEnd;
		setup.combStart = setup.combEnd;
		setup.dampStart = setup.dampEnd;
		setup.h0Start = setup.h0End;
		setup.h1Start = setup.h1End;
	}
	return setup;
}

/// Sympathetic strings and chords: four strings side by side. in = the struck string's input, out = the mix. The
/// damping and brightness taps step once a block: Rings ramps them, which only matters while a knob turns.
/// kWarm: the lines aren't full yet. A fresh voice's lines hold whatever the memory held (nothing is cleared at note
/// on: clearing four lines for each of several voices struck at once took long enough in one render for the Deluge to
/// cull every voice), so any read further back than what's been written reads silence instead.
template <bool kWarm>
void stringsCoreLoop(Voice& v, const LaneSetup& setup, const float* in, float* out, int32_t n, float sympGain,
                     const int32_t* effQ, const int32_t* effStepQ, const int32_t* combQ, const int32_t* combStepQ);

/// The same for two strings (Rings at 4-voice polyphony), on half vectors: about half the work of four
template <bool kWarm>
void stringsCoreLoop2(Voice& v, const LaneSetup& setup, const float* in, float* out, int32_t n, float sympGain,
                      const int32_t* effQ, const int32_t* effStepQ, const int32_t* combQ, const int32_t* combStepQ);

void stringsCore(Voice& v, const LaneSetup& setup, const float* in, float* out, int32_t n, float sympGain,
                 int32_t numStrings) {
	const float invN = 1.f / static_cast<float>(n);
	// Fixed point delays, 16 fractional bits: the integer registers address the lines, NEON takes the fractions
	alignas(16) int32_t effQ[4];
	alignas(16) int32_t effStepQ[4];
	alignas(16) int32_t combQ[4];
	alignas(16) int32_t combStepQ[4];
	const float32x4_t kQ = vdupq_n_f32(65536.f);
	const float32x4_t effStep = vmulq_n_f32(vsubq_f32(setup.effEnd, setup.effStart), invN);
	const float32x4_t combStep = vmulq_n_f32(vsubq_f32(setup.combEnd, setup.combStart), invN);
	vst1q_s32(effQ, vcvtq_s32_f32(vmulq_f32(setup.effStart, kQ)));
	vst1q_s32(effStepQ, vcvtq_s32_f32(vmulq_f32(effStep, kQ)));
	vst1q_s32(combQ, vcvtq_s32_f32(vmulq_f32(setup.combStart, kQ)));
	vst1q_s32(combStepQ, vcvtq_s32_f32(vmulq_f32(combStep, kQ)));

	// Whether anything they'll read is further back than what's been written
	int32_t maxRead = 0;
	for (int32_t l = 0; l < numStrings; l++) {
		const int32_t e = std::max(effQ[l], effQ[l] + effStepQ[l] * n) >> 16;
		const int32_t c = std::max(combQ[l], combQ[l] + combStepQ[l] * n) >> 16;
		maxRead = std::max({maxRead, e + 2, c});
	}
	const bool warm = maxRead > v.valid;
	if (numStrings == 2) {
		if (warm) {
			stringsCoreLoop2<true>(v, setup, in, out, n, sympGain, effQ, effStepQ, combQ, combStepQ);
		}
		else {
			stringsCoreLoop2<false>(v, setup, in, out, n, sympGain, effQ, effStepQ, combQ, combStepQ);
		}
	}
	else if (warm) {
		stringsCoreLoop<true>(v, setup, in, out, n, sympGain, effQ, effStepQ, combQ, combStepQ);
	}
	else {
		stringsCoreLoop<false>(v, setup, in, out, n, sympGain, effQ, effStepQ, combQ, combStepQ);
	}
}

template <bool kWarm>
void stringsCoreLoop(Voice& v, const LaneSetup& setup, const float* in, float* out, int32_t n, float sympGain,
                     const int32_t* effQ, const int32_t* effStepQ, const int32_t* combQ, const int32_t* combStepQ) {
	const float32x4_t damp = setup.dampEnd;
	const float32x4_t h0 = vmulq_f32(setup.h0End, damp);
	const float32x4_t h1 = vmulq_f32(setup.h1End, damp);
	const float32x4_t g = setup.g;
	const float32x4_t rg = setup.rg;
	const float32x4_t h = setup.h;
	float32x4_t x1 = vld1q_f32(v.firX1);
	float32x4_t x2 = vld1q_f32(v.firX2);
	float32x4_t s1 = vld1q_f32(v.iirS1);
	float32x4_t s2 = vld1q_f32(v.iirS2);
	float32x4_t symp = vdupq_n_f32(v.sympPrev);
	float* const acc = v.scratch;

	float* const line0 = v.line[0];
	float* const line1 = v.line[1];
	float* const line2 = v.line[2];
	float* const line3 = v.line[3];
	uint32_t wp = v.wp;
	int32_t e0 = effQ[0], e1 = effQ[1], e2 = effQ[2], e3 = effQ[3];
	int32_t c0 = combQ[0], c1 = combQ[1], c2 = combQ[2], c3 = combQ[3];
	const int32_t de0 = effStepQ[0], de1 = effStepQ[1], de2 = effStepQ[2], de3 = effStepQ[3];
	const int32_t dc0 = combStepQ[0], dc1 = combStepQ[1], dc2 = combStepQ[2], dc3 = combStepQ[3];

	for (int32_t i = 0; i < n; i++) {
		e0 += de0;
		e1 += de1;
		e2 += de2;
		e3 += de3;
		c0 += dc0;
		c1 += dc1;
		c2 += dc2;
		c3 += dc3;
		// The fractions go across to NEON (that way round costs nothing), the integer parts address the lines
		uint32x4_t effV = vdupq_n_u32(static_cast<uint32_t>(e0));
		effV = vsetq_lane_u32(static_cast<uint32_t>(e1), effV, 1);
		effV = vsetq_lane_u32(static_cast<uint32_t>(e2), effV, 2);
		effV = vsetq_lane_u32(static_cast<uint32_t>(e3), effV, 3);
		uint32x4_t combV = vdupq_n_u32(static_cast<uint32_t>(c0));
		combV = vsetq_lane_u32(static_cast<uint32_t>(c1), combV, 1);
		combV = vsetq_lane_u32(static_cast<uint32_t>(c2), combV, 2);
		combV = vsetq_lane_u32(static_cast<uint32_t>(c3), combV, 3);

		// Four taps around each string's delay, loaded straight into a vector per tap
		float32x4x4_t taps;
		float32x4x2_t pick;
		taps = vld4q_lane_f32(line0 + ((wp + (e0 >> 16) - 1) & kLineMask), taps, 0);
		taps = vld4q_lane_f32(line1 + ((wp + (e1 >> 16) - 1) & kLineMask), taps, 1);
		taps = vld4q_lane_f32(line2 + ((wp + (e2 >> 16) - 1) & kLineMask), taps, 2);
		taps = vld4q_lane_f32(line3 + ((wp + (e3 >> 16) - 1) & kLineMask), taps, 3);
		const float32x4_t xm1 = taps.val[0];
		const float32x4_t x0 = taps.val[1];
		const float32x4_t xp1 = taps.val[2];
		const float32x4_t xp2 = taps.val[3];
		const float32x4_t f = vcvtq_n_f32_u32(vshlq_n_u32(effV, 16), 32);
		// Hermite (stmlib DelayLine::ReadHermite)
		const float32x4_t c = vmulq_n_f32(vsubq_f32(xp1, xm1), 0.5f);
		const float32x4_t vv = vsubq_f32(x0, xp1);
		const float32x4_t w = vaddq_f32(c, vv);
		const float32x4_t a = vmlaq_n_f32(vaddq_f32(w, vv), vsubq_f32(xp2, x0), 0.5f);
		const float32x4_t bNeg = vaddq_f32(w, a);
		float32x4_t sv = vmlaq_f32(c, vmlsq_f32(bNeg, a, f), vnegq_f32(f)); // c + (a f - b) f
		sv = vmlaq_f32(x0, sv, f);
		if constexpr (kWarm) {
			// A tap k samples back holds string data once k samples have been written
			const int32x4_t written = vdupq_n_s32(v.valid + i);
			const int32x4_t lastTap = vaddq_s32(vshrq_n_s32(vreinterpretq_s32_u32(effV), 16), vdupq_n_s32(2));
			sv = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(sv), vcleq_s32(lastTap, written)));
		}

		// The struck string hears the input, the others it, a sample late
		sv = vaddq_f32(sv, vld1q_lane_f32(in + i, symp, 0));
		// FIR damping, damping folded into its taps
		const float32x4_t fir = vmlaq_f32(vmulq_f32(h1, vaddq_f32(sv, x2)), h0, x1);
		x2 = x1;
		x1 = sv;
		// IIR damping, low pass
		const float32x4_t hp = vmulq_f32(vsubq_f32(vmlsq_f32(fir, rg, s1), s2), h);
		const float32x4_t ghp = vmulq_f32(g, hp);
		const float32x4_t bp = vaddq_f32(ghp, s1);
		s1 = vaddq_f32(ghp, bp);
		const float32x4_t gbp = vmulq_f32(g, bp);
		const float32x4_t lp = vaddq_f32(gbp, s2);
		s2 = vaddq_f32(gbp, lp);

		// Into the lines
		vst1q_lane_f32(line0 + wp, lp, 0);
		vst1q_lane_f32(line1 + wp, lp, 1);
		vst1q_lane_f32(line2 + wp, lp, 2);
		vst1q_lane_f32(line3 + wp, lp, 3);
		if (wp < kLineGuard) {
			vst1q_lane_f32(line0 + wp + kLineSize, lp, 0);
			vst1q_lane_f32(line1 + wp + kLineSize, lp, 1);
			vst1q_lane_f32(line2 + wp + kLineSize, lp, 2);
			vst1q_lane_f32(line3 + wp + kLineSize, lp, 3);
		}
		wp = (wp - 1) & kLineMask;

		// The pickup: a second, linear read
		pick = vld2q_lane_f32(line0 + ((wp + (c0 >> 16)) & kLineMask), pick, 0);
		pick = vld2q_lane_f32(line1 + ((wp + (c1 >> 16)) & kLineMask), pick, 1);
		pick = vld2q_lane_f32(line2 + ((wp + (c2 >> 16)) & kLineMask), pick, 2);
		pick = vld2q_lane_f32(line3 + ((wp + (c3 >> 16)) & kLineMask), pick, 3);
		const float32x4_t cf = vcvtq_n_f32_u32(vshlq_n_u32(combV, 16), 32);
		float32x4_t pickup = vmlaq_f32(pick.val[0], vsubq_f32(pick.val[1], pick.val[0]), cf);
		if constexpr (kWarm) {
			// Counted from the new write point, where the sample just written is 1 back
			const int32x4_t written = vdupq_n_s32(v.valid + i + 1);
			const int32x4_t lastTap = vaddq_s32(vshrq_n_s32(vreinterpretq_s32_u32(combV), 16), vdupq_n_s32(1));
			pickup = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(pickup), vcleq_s32(lastTap, written)));
		}

		// Mono: each string minus its pickup, summed after the loop
		const float32x4_t diff = vsubq_f32(lp, pickup);
		vst1q_f32(acc + 4 * i, diff);
		symp = vmulq_n_f32(vdupq_lane_f32(vget_low_f32(diff), 0), sympGain);
	}
	// Sum each sample's four strings
	const int32_t padded = (n + 3) & ~3;
	for (int32_t i = n; i < padded; i++) {
		vst1q_f32(acc + 4 * i, vdupq_n_f32(0.f));
	}
	for (int32_t i = 0; i < padded; i += 4) {
		const float32x4x4_t q = vld4q_f32(acc + 4 * i);
		vst1q_f32(out + i, vaddq_f32(vaddq_f32(q.val[0], q.val[1]), vaddq_f32(q.val[2], q.val[3])));
	}
	v.wp = wp;
	v.valid = std::min(v.valid + n, kLineSize - 1);
	v.sympPrev = vgetq_lane_f32(symp, 0);
	vst1q_f32(v.firX1, x1);
	vst1q_f32(v.firX2, x2);
	vst1q_f32(v.iirS1, s1);
	vst1q_f32(v.iirS2, s2);
}

template <bool kWarm>
void stringsCoreLoop2(Voice& v, const LaneSetup& setup, const float* in, float* out, int32_t n, float sympGain,
                      const int32_t* effQ, const int32_t* effStepQ, const int32_t* combQ, const int32_t* combStepQ) {
	const float32x2_t damp = vget_low_f32(setup.dampEnd);
	const float32x2_t h0 = vmul_f32(vget_low_f32(setup.h0End), damp);
	const float32x2_t h1 = vmul_f32(vget_low_f32(setup.h1End), damp);
	const float32x2_t g = vget_low_f32(setup.g);
	const float32x2_t rg = vget_low_f32(setup.rg);
	const float32x2_t h = vget_low_f32(setup.h);
	float32x2_t x1 = vld1_f32(v.firX1);
	float32x2_t x2 = vld1_f32(v.firX2);
	float32x2_t s1 = vld1_f32(v.iirS1);
	float32x2_t s2 = vld1_f32(v.iirS2);
	float32x2_t symp = vdup_n_f32(v.sympPrev);

	float* const line0 = v.line[0];
	float* const line1 = v.line[1];
	uint32_t wp = v.wp;
	int32_t e0 = effQ[0], e1 = effQ[1];
	int32_t c0 = combQ[0], c1 = combQ[1];
	const int32_t de0 = effStepQ[0], de1 = effStepQ[1];
	const int32_t dc0 = combStepQ[0], dc1 = combStepQ[1];

	for (int32_t i = 0; i < n; i++) {
		e0 += de0;
		e1 += de1;
		c0 += dc0;
		c1 += dc1;
		uint32x2_t effV = vdup_n_u32(static_cast<uint32_t>(e0));
		effV = vset_lane_u32(static_cast<uint32_t>(e1), effV, 1);
		uint32x2_t combV = vdup_n_u32(static_cast<uint32_t>(c0));
		combV = vset_lane_u32(static_cast<uint32_t>(c1), combV, 1);

		float32x2x4_t taps;
		float32x2x2_t pick;
		taps = vld4_lane_f32(line0 + ((wp + (e0 >> 16) - 1) & kLineMask), taps, 0);
		taps = vld4_lane_f32(line1 + ((wp + (e1 >> 16) - 1) & kLineMask), taps, 1);
		const float32x2_t xm1 = taps.val[0];
		const float32x2_t x0 = taps.val[1];
		const float32x2_t xp1 = taps.val[2];
		const float32x2_t xp2 = taps.val[3];
		const float32x2_t f = vcvt_n_f32_u32(vshl_n_u32(effV, 16), 32);
		const float32x2_t c = vmul_n_f32(vsub_f32(xp1, xm1), 0.5f);
		const float32x2_t vv = vsub_f32(x0, xp1);
		const float32x2_t w = vadd_f32(c, vv);
		const float32x2_t a = vmla_n_f32(vadd_f32(w, vv), vsub_f32(xp2, x0), 0.5f);
		const float32x2_t bNeg = vadd_f32(w, a);
		float32x2_t sv = vmla_f32(c, vmls_f32(bNeg, a, f), vneg_f32(f));
		sv = vmla_f32(x0, sv, f);
		if constexpr (kWarm) {
			const int32x2_t written = vdup_n_s32(v.valid + i);
			const int32x2_t lastTap = vadd_s32(vshr_n_s32(vreinterpret_s32_u32(effV), 16), vdup_n_s32(2));
			sv = vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(sv), vcle_s32(lastTap, written)));
		}

		sv = vadd_f32(sv, vld1_lane_f32(in + i, symp, 0));
		const float32x2_t fir = vmla_f32(vmul_f32(h1, vadd_f32(sv, x2)), h0, x1);
		x2 = x1;
		x1 = sv;
		const float32x2_t hp = vmul_f32(vsub_f32(vmls_f32(fir, rg, s1), s2), h);
		const float32x2_t ghp = vmul_f32(g, hp);
		const float32x2_t bp = vadd_f32(ghp, s1);
		s1 = vadd_f32(ghp, bp);
		const float32x2_t gbp = vmul_f32(g, bp);
		const float32x2_t lp = vadd_f32(gbp, s2);
		s2 = vadd_f32(gbp, lp);

		vst1_lane_f32(line0 + wp, lp, 0);
		vst1_lane_f32(line1 + wp, lp, 1);
		if (wp < kLineGuard) {
			vst1_lane_f32(line0 + wp + kLineSize, lp, 0);
			vst1_lane_f32(line1 + wp + kLineSize, lp, 1);
		}
		wp = (wp - 1) & kLineMask;

		pick = vld2_lane_f32(line0 + ((wp + (c0 >> 16)) & kLineMask), pick, 0);
		pick = vld2_lane_f32(line1 + ((wp + (c1 >> 16)) & kLineMask), pick, 1);
		const float32x2_t cf = vcvt_n_f32_u32(vshl_n_u32(combV, 16), 32);
		float32x2_t pickup = vmla_f32(pick.val[0], vsub_f32(pick.val[1], pick.val[0]), cf);
		if constexpr (kWarm) {
			const int32x2_t written = vdup_n_s32(v.valid + i + 1);
			const int32x2_t lastTap = vadd_s32(vshr_n_s32(vreinterpret_s32_u32(combV), 16), vdup_n_s32(1));
			pickup = vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(pickup), vcle_s32(lastTap, written)));
		}

		// Mono: each string minus its pickup, both summed
		const float32x2_t diff = vsub_f32(lp, pickup);
		vst1_lane_f32(out + i, vpadd_f32(diff, diff), 0);
		symp = vmul_n_f32(vdup_lane_f32(diff, 0), sympGain);
	}
	v.wp = wp;
	v.valid = std::min(v.valid + n, kLineSize - 1);
	v.sympPrev = vget_lane_f32(symp, 0);
	vst1_f32(v.firX1, x1);
	vst1_f32(v.firX2, x2);
	vst1_f32(v.iirS1, s1);
	vst1_f32(v.iirS2, s2);
}

/// Sympathetic strings and chords: four strings a voice (Rings at 2-voice polyphony) or two (at 4-voice)
[[gnu::noinline]] void renderSympathetic(Voice& v, const Settings& s, float f0, bool strike, float* out, int32_t n) {
	const int32_t numStrings = (s.strings >= 4) ? 4 : 2;
	alignas(16) float x[kMaxRender + 8];
	stringInput(v, s, f0, strike, numStrings, x, n);
	// One block for the whole render: Rings' control rate block is 24 samples, but its settings only interpolate
	const LaneSetup setup = setupLanes(v, s, f0, numStrings, n);
	stringsCore(v, setup, x, out, n, 0.2f / static_cast<float>(numStrings), numStrings);
}

/// Four taps of a line around a delay, Hermite interpolated (stmlib DelayLine::ReadHermite). The guard samples at the
/// end of the line let the taps run past it.
[[gnu::always_inline]] inline float hermite(const float* line, uint32_t wp, float d) {
	const auto di = static_cast<uint32_t>(d);
	const float fr = d - static_cast<float>(di);
	const float* q = line + ((wp + di - 1) & kLineMask);
	const float xm1 = q[0], x0 = q[1], x1 = q[2], x2 = q[3];
	const float c = (x1 - xm1) * 0.5f;
	const float vv = x0 - x1;
	const float w = c + vv;
	const float a = w + vv + (x2 - x0) * 0.5f;
	const float bNeg = w + a;
	return (((a * fr) - bNeg) * fr + c) * fr + x0;
}

/// The same with the delay in 16.16 fixed point, which the integer registers can address with directly
[[gnu::always_inline]] inline float hermiteQ(const float* line, uint32_t wp, int32_t dq) {
	const auto di = static_cast<uint32_t>(dq >> 16);
	const float fr = static_cast<float>(dq & 0xFFFF) * (1.f / 65536.f);
	const float* q = line + ((wp + di - 1) & kLineMask);
	const float xm1 = q[0], x0 = q[1], x1 = q[2], x2 = q[3];
	const float c = (x1 - xm1) * 0.5f;
	const float vv = x0 - x1;
	const float w = c + vv;
	const float a = w + vv + (x2 - x0) * 0.5f;
	const float bNeg = w + a;
	return (((a * fr) - bNeg) * fr + c) * fr + x0;
}

/// One block of the inharmonic string. kSplit: the dispersion's all pass is in the loop for the whole block.
/// kNoise / kCurve: noise (structure over about 0.81) or the curved bridge (under 0.24) move the delay every sample.
/// Without either the delay only glides, so it runs in fixed point, and each is left out where it does nothing.
template <bool kSplit, bool kNoise, bool kCurve>
void stringBlock(Voice& v, const LaneSetup& setup, const Dispersion& d0, const Dispersion& d1, float noiseFilter,
                 const float* in, float* out, int32_t m) {
	alignas(16) float effStart[4], effEnd[4], combStart[4], combEnd[4], dampStart[4], dampEnd[4], h0e[4], h1e[4];
	alignas(16) float gs[4], rgs[4], hs[4];
	vst1q_f32(effStart, setup.effStart);
	vst1q_f32(effEnd, setup.effEnd);
	vst1q_f32(combStart, setup.combStart);
	vst1q_f32(combEnd, setup.combEnd);
	vst1q_f32(dampStart, setup.dampStart);
	vst1q_f32(dampEnd, setup.dampEnd);
	vst1q_f32(h0e, setup.h0End);
	vst1q_f32(h1e, setup.h1End);
	vst1q_f32(gs, setup.g);
	vst1q_f32(rgs, setup.rg);
	vst1q_f32(hs, setup.h);
	const float invM = 1.f / static_cast<float>(m);
	float eff = effStart[0];
	const float effStep = (effEnd[0] - effStart[0]) * invM;
	// The pickup reads in fixed point, so its position never goes through a float to integer conversion
	int32_t combQ = static_cast<int32_t>(combStart[0] * 65536.f);
	const auto combStepQ = static_cast<int32_t>((combEnd[0] - combStart[0]) * invM * 65536.f);
	float damp = dampStart[0];
	const float dampStep = (dampEnd[0] - dampStart[0]) * invM;
	const float h0 = h0e[0];
	const float h1 = h1e[0];
	float stretchPoint = d0.stretchPoint;
	const float stretchStep = (d1.stretchPoint - d0.stretchPoint) * invM;
	float noiseAmount = d0.noiseAmount;
	const float noiseAmountStep = (d1.noiseAmount - d0.noiseAmount) * invM;
	float bridgeCurving = d0.bridgeCurving;
	const float bridgeCurvingStep = (d1.bridgeCurving - d0.bridgeCurving) * invM;
	float acBlocking = d0.acBlocking;
	const float acBlockingStep = (d1.acBlocking - d0.acBlocking) * invM;
	float apGain = d0.apGain;
	const float apGainStep = (d1.apGain - d0.apGain) * invM;
	const float noiseScale = 1.f / (0.2f + noiseFilter);
	const float g = gs[0];
	const float rg = rgs[0];
	const float h = hs[0];
	float x1 = v.firX1[0], x2 = v.firX2[0], s1 = v.iirS1[0], s2 = v.iirS2[0];
	float dispNoise = v.dispNoise;
	float curved = v.curvedBridge;
	float sdcX = v.sdcX, sdcY = v.sdcY;
	uint32_t wp = v.wp;
	float* const line = v.line[0];
	float* const stretch = v.stretch;
	constexpr float kDcPole = 1.f - 20.f / kSampleRate;
	constexpr float kMaxDelay = kLineSize - 4.f;
	// Fixed point delays for the unmodulated string: the whole delay, or the main part and the all pass's
	const float apStart = effStart[0] * d0.stretchPoint;
	const float apEnd = effEnd[0] * d1.stretchPoint;
	const float mainStart = kSplit ? effStart[0] - apStart : effStart[0];
	const float mainEnd = kSplit ? effEnd[0] - apEnd : effEnd[0];
	int32_t mainQ = static_cast<int32_t>(mainStart * 65536.f);
	const auto mainStepQ = static_cast<int32_t>((mainEnd - mainStart) * invM * 65536.f);
	int32_t apQ = static_cast<int32_t>(apStart * 65536.f);
	const auto apStepQ = static_cast<int32_t>((apEnd - apStart) * invM * 65536.f);
	const int32_t valid = v.valid;
	const bool warm = valid < kLineSize - 1;
	for (int32_t i = 0; i < m; i++) {
		combQ += combStepQ;
		damp += dampStep;
		apGain += apGainStep;
		float sv;
		// A read reaching further back than what's been written reads silence, before anything else uses it
		auto unwritten = [&](int32_t lastTap) { return warm && lastTap > valid + i; };
		if constexpr (!kNoise && !kCurve) {
			mainQ += mainStepQ;
			sv = hermiteQ(line, wp, mainQ);
			if (unwritten((mainQ >> 16) + 2)) {
				sv = 0.f;
			}
			if constexpr (kSplit) {
				apQ += apStepQ;
				const float read =
				    stretch[((wp & (kStretchSize - 1)) + static_cast<uint32_t>(apQ >> 16)) & (kStretchSize - 1)];
				const float write = sv + apGain * read;
				stretch[wp & (kStretchSize - 1)] = write;
				sv = -write * apGain + read;
			}
		}
		else {
			eff += effStep;
			stretchPoint += stretchStep;
			noiseAmount += noiseAmountStep;
			bridgeCurving += bridgeCurvingStep;
			acBlocking += acBlockingStep;
			float fm = 1.f;
			if constexpr (kNoise) {
				const float noise = v.noise() * noiseScale;
				dispNoise += noiseFilter * (noise - dispNoise);
				fm += dispNoise * noiseAmount;
			}
			if constexpr (kCurve) {
				fm -= curved * bridgeCurving;
			}
			float delay = eff * fm;
			// Kept between 2 and the line's length without a compare
			delay = 2.f + positivePart(delay - 2.f);
			delay = kMaxDelay - positivePart(kMaxDelay - delay);
			if constexpr (kSplit) {
				const float apDelay = delay * stretchPoint;
				sv = hermite(line, wp, delay - apDelay);
				if (unwritten(static_cast<int32_t>(delay - apDelay) + 2)) {
					sv = 0.f;
				}
				// stmlib DelayLine::Allpass on the stretch line
				const float read =
				    stretch[((wp & (kStretchSize - 1)) + static_cast<uint32_t>(apDelay)) & (kStretchSize - 1)];
				const float write = sv + apGain * read;
				stretch[wp & (kStretchSize - 1)] = write;
				sv = -write * apGain + read;
			}
			else {
				sv = hermite(line, wp, delay);
				if (unwritten(static_cast<int32_t>(delay) + 2)) {
					sv = 0.f;
				}
			}
			if constexpr (kCurve) {
				// AC blocking of the curved bridge
				const float old = sdcX;
				sdcX = sv;
				sdcY = sdcY * kDcPole + sdcX - old;
				sv += acBlocking * (sdcY - sv);
				// Rings: value = |s| - 0.025, curved = (|value| + value) * (s > 0 ? 1 : -1.5)
				curved = 2.f * positivePart(sv - 0.025f) - 3.f * positivePart(-sv - 0.025f);
			}
		}

		sv += in[i];
		const float fir = damp * (h0 * x1 + h1 * (sv + x2));
		x2 = x1;
		x1 = sv;
		const float hp = (fir - rg * s1 - s2) * h;
		const float bp = g * hp + s1;
		s1 = g * hp + bp;
		const float lp = g * bp + s2;
		s2 = g * bp + lp;
		writeLine(line, wp, lp);
		wp = (wp - 1) & kLineMask;
		const float* q = line + ((wp + static_cast<uint32_t>(combQ >> 16)) & kLineMask);
		const float cfr = static_cast<float>(combQ & 0xFFFF) * (1.f / 65536.f);
		float pickup = q[0] + (q[1] - q[0]) * cfr;
		if (warm && (combQ >> 16) + 1 > valid + i + 1) {
			pickup = 0.f; // Not written yet
		}
		out[i] = lp - pickup;
	}
	v.firX1[0] = x1;
	v.firX2[0] = x2;
	v.iirS1[0] = s1;
	v.iirS2[0] = s2;
	v.dispNoise = dispNoise;
	v.curvedBridge = curved;
	v.sdcX = sdcX;
	v.sdcY = sdcY;
	v.wp = wp;
	v.valid = std::min(v.valid + m, kLineSize - 1);
}

/// The inharmonic string: one string with dispersion, as Rings' String::ProcessInternal<true>, on line 0
[[gnu::noinline]] void renderString(Voice& v, const Settings& s, float f0, bool strike, float* out, int32_t n) {
	alignas(16) float x[kMaxRender + 8];
	stringInput(v, s, f0, strike, 1, x, n);
	if (!v.stretchClear) {
		std::memset(v.stretch, 0, sizeof(v.stretch));
		v.stretchClear = true;
	}
	const float structure = s.structure;
	const float dispersionTarget = (structure < 0.24f)   ? (structure - 0.24f) * 4.166f
	                               : (structure > 0.26f) ? (structure - 0.26f) * 1.35135f
	                                                     : 0.f;
	const float noiseFilter = semitonesToRatio((s.brightness - 1.f) * 48.f);
	// One block for the whole render, as for the sympathetic strings
	const LaneSetup setup = setupLanes(v, s, f0, 1, n);
	const float effStart = vgetq_lane_f32(setup.effStart, 0);
	const float effEnd = vgetq_lane_f32(setup.effEnd, 0);
	// Nothing is cleared ahead: until the line is full, reads further back than what's been written read silence
	// (stringBlock)
	if (dispersionTarget != v.dispersion.value) {
		v.dispersion = Dispersion(dispersionTarget);
	}
	const Dispersion& d0 = v.prevDispersionSettings;
	const Dispersion& d1 = v.dispersion;
	// Rings only splits the delay through the all pass while both parts are 4 samples or more
	auto splits = [](float e, const Dispersion& d) {
		const float ap = e * d.stretchPoint;
		return ap >= 4.f && e - ap >= 4.f;
	};
	const bool split = splits(effStart, d0) && splits(effEnd, d1);
	const bool noisy = d0.noisy || d1.noisy;
	const bool curving = d0.curved || d1.curved;
	if (!curving) {
		v.curvedBridge = 0.f; // The bridge rests
	}
	using Block =
	    void (*)(Voice&, const LaneSetup&, const Dispersion&, const Dispersion&, float, const float*, float*, int32_t);
	static constexpr Block kBlocks[8] = {
	    stringBlock<false, false, false>, stringBlock<false, false, true>, stringBlock<false, true, false>,
	    stringBlock<false, true, true>,   stringBlock<true, false, false>, stringBlock<true, false, true>,
	    stringBlock<true, true, false>,   stringBlock<true, true, true>,
	};
	kBlocks[(split ? 4 : 0) + (noisy ? 2 : 0) + (curving ? 1 : 0)](v, setup, d0, d1, noiseFilter, x, out, n);
	v.prevDispersionSettings = d1;
}

// FM -----------------------------------------------------------------------------------------------------------------

[[gnu::noinline]] void renderFm(Voice& v, const Settings& s, float f0, bool strike, float* out, int32_t n) {
	alignas(16) float x[kMaxRender + 8];
	const bool hasInput = bowNoise(v, x, n, s.bow);
	if (strike) {
		v.fmAmpEnv = 1.f; // FMVoice::TriggerInternalEnvelope
		v.fmBrightEnv = 1.f;
	}
	const float damping = s.damping;
	const float envelopeAmount = (damping < 0.9f) ? 1.f : (1.f - damping) * 10.f;
	// 1 - 0.001^(1 / rt60)
	const float ampRt60 = 0.1f * semitonesToRatio(damping * 96.f) * kSampleRate;
	const float ampDecay = -exp2Minus1(-9.965784f / ampRt60);
	const float brightRt60 = 0.1f * semitonesToRatio(damping * 84.f) * kSampleRate;
	const float brightDecay = -exp2Minus1(-9.965784f / brightRt60);
	const float ratio = lookup(kFmQuantizer, s.structure * 128.f);
	const float modulatorTarget = std::min(f0 * semitonesToRatio(ratio), 0.5f);
	const float feedbackTarget = (s.position - 0.5f) * 2.f;
	const float phaseFeedback = (feedbackTarget < 0.f) ? 0.5f * feedbackTarget * feedbackTarget : 0.f;
	if (v.fmPrevCarrierInc < 0.f) {
		v.fmPrevCarrierInc = f0;
		v.fmPrevModInc = modulatorTarget;
		v.fmPrevBright = s.brightness;
		v.fmPrevFeedback = feedbackTarget;
	}
	const float invN = 1.f / static_cast<float>(n);
	float carrierInc = v.fmPrevCarrierInc;
	const float carrierStep = (f0 - carrierInc) * invN;
	float modInc = v.fmPrevModInc;
	const float modStep = (modulatorTarget - modInc) * invN;
	float feedback = v.fmPrevFeedback;
	const float feedbackStep = (feedbackTarget - feedback) * invN;
	// fm amount's range from brightness^2, interpolated across the render as Rings interpolates brightness
	auto range = [](float b, float& lo, float& hi) {
		b *= b;
		lo = (b < 0.5f) ? 0.f : b * 2.f - 1.f;
		hi = (b < 0.5f) ? 2.f * b : 1.f;
	};
	float lo0, hi0, lo1, hi1;
	range(v.fmPrevBright, lo0, hi0);
	range(s.brightness, lo1, hi1);
	float lo = lo0, hi = hi0;
	const float loStep = (lo1 - lo0) * invN;
	const float hiStep = (hi1 - hi0) * invN;
	v.fmPrevCarrierInc = f0;
	v.fmPrevModInc = modulatorTarget;
	v.fmPrevFeedback = feedbackTarget;
	v.fmPrevBright = s.brightness;

	// Rings' envelope follower (three band detectors and their centroid) on the bow. The bow is white noise, which the
	// follower reads, once settled, as an envelope of 1.16 times its level and a centroid of 0.906 (measured on Rings'
	// own Follower): so it is followed a render at a time, at the detectors' and centroid's own rates, rather than
	// run per sample.
	const float bowLevel = hasInput ? bowGain(s.bow) : 0.f;
	const bool follow = hasInput || v.flEnvelope > 1e-6f;
	float followEnv = 0.f;
	float followCentroid = 0.f;
	if (follow) {
		constexpr float kWhiteEnvelope = 1.16031f;
		constexpr float kWhiteCentroid = 0.90646f;
		constexpr float kRise = 0.0114f; // The middle detector's attack, sqrt(160 * 1600) / sr
		constexpr float kFall = 0.0036f; // and decay, 160 / sr
		const float target = kWhiteEnvelope * bowLevel;
		const float rate = (target > v.flEnvelope) ? kRise : kFall;
		v.flEnvelope += (target - v.flEnvelope) * (1.f - exp2Fast(log2Fast(1.f - rate) * static_cast<float>(n)));
		const float centroidTarget = hasInput ? kWhiteCentroid : 0.f;
		const float centroidRate = (centroidTarget > v.flCentroid) ? 0.05f : 0.001f;
		v.flCentroid +=
		    (centroidTarget - v.flCentroid) * (1.f - exp2Fast(log2Fast(1.f - centroidRate) * static_cast<float>(n)));
		followEnv = v.flEnvelope;
		followCentroid = v.flCentroid;
	}

	float ampEnv = v.fmAmpEnv;
	float brightEnv = v.fmBrightEnv;
	float fmAmount = v.fmAmount;
	float gain = v.fmGain;
	float prev = v.fmPrev;
	float carrier = v.fmCarrier;
	float modulator = v.fmModulator;
	// The modulator's own feedback, worked out at both ends and interpolated, so there's no compare per sample
	float modulatorFb = 0.25f * positivePart(feedback) * positivePart(feedback);
	const float modulatorFbEnd = 0.25f * positivePart(feedbackTarget) * positivePart(feedbackTarget);
	const float modulatorFbStep = (modulatorFbEnd - modulatorFb) * invN;
	(void)feedbackStep;
	const float ampKeep = 1.f - ampDecay;
	const float brightKeep = 1.f - brightDecay;
	for (int32_t i = 0; i < n; i++) {
		if (follow) {
			// SLOPE: fast up, slow down
			const float a = followEnv;
			const float b = followCentroid * 2.f * a * (2.f - a);
			float e = a - ampEnv;
			ampEnv += ampDecay * e + (0.05f - ampDecay) * positivePart(e);
			e = b - brightEnv;
			brightEnv += brightDecay * e + (0.01f - brightDecay) * positivePart(e);
		}
		else {
			// No input: the envelopes only fall
			ampEnv *= ampKeep;
			brightEnv *= brightKeep;
		}
		lo += loStep;
		hi += hiStep;
		const float fmEnvelope = 0.5f + envelopeAmount * (brightEnv - 0.5f);
		const float target = (lo + hi * fmEnvelope) * 2.f;
		// SLEW: at most d a sample
		const float d = 0.005f + hi * 0.015f;
		const float diff = target - fmAmount;
		fmAmount += 0.5f * (std::fabs(diff + d) - std::fabs(diff - d));

		modInc += modStep;
		carrierInc += carrierStep;
		modulator += modInc * (1.f + prev * phaseFeedback);
		modulator -= static_cast<float>(static_cast<int32_t>(modulator));
		carrier += carrierInc;
		carrier -= static_cast<float>(static_cast<int32_t>(carrier));
		modulatorFb += modulatorFbStep;
		const float mod = sine2Pi(modulator + modulatorFb * prev);
		const float car = sine2Pi(carrier + fmAmount * mod);
		prev += 0.1f * (car - prev);
		const float g = 1.f + envelopeAmount * (ampEnv - 1.f);
		gain += (0.005f + 0.045f * fmAmount) * (g - gain);
		// Mono: Rings' out minus aux leaves the carrier
		out[i] = car * gain;
	}
	v.fmAmpEnv = ampEnv;
	v.fmBrightEnv = brightEnv;
	v.fmAmount = fmAmount;
	v.fmGain = gain;
	v.fmPrev = prev;
	v.fmCarrier = carrier;
	v.fmModulator = modulator;
}

// Output -------------------------------------------------------------------------------------------------------------

/// Rings' limiter (gain into a peak follower, 0.8 of full scale, soft limit), eight samples a step, then the drive, to
/// q31. All on NEON, no compares. Returns the render's peak, before the clipper.
[[gnu::noinline]] float outputStage(Voice& v, float* x, int32_t* out, int32_t n, float preGain, float drive) {
	const int32_t padded = (n + 7) & ~7;
	for (int32_t i = n; i < padded; i++) {
		x[i] = 0.f;
	}
	// Rings' follower moves 0.05 up / 0.00002 down a sample: eight samples' worth a step
	const float32x2_t kRelease = vdup_n_f32(1.59981e-4f);              // 1 - (1 - 0.00002)^8
	const float32x2_t kAttack = vdup_n_f32(0.33657776f - 1.59981e-4f); // (1 - 0.95^8) - release
	const float32x2_t zero = vdup_n_f32(0.f);
	const float32x2_t one = vdup_n_f32(1.f);
	const float driveGain = 1.f + drive * drive * 15.f;
	// The drive's level back down, folded into the clipper's denominator
	const float makeup = 1.f + drive * 1.5f;
	const float32x4_t den27 = vdupq_n_f32(27.f * makeup);
	const float32x4_t den9 = vdupq_n_f32(9.f * makeup);
	const float32x4_t limit3 = vdupq_n_f32(3.f);
	const float32x4_t limitM3 = vdupq_n_f32(-3.f);
	const float32x4_t c27 = vdupq_n_f32(27.f);
	float32x2_t peak = vdup_n_f32(v.limiterPeak);
	float32x2_t level = zero;
	const float32x2_t scale = vdup_n_f32(0.8f * driveGain * preGain);
	alignas(16) int32_t q[kMaxRender + 8];
	auto clip = [&](float32x4_t y) {
		y = vmaxq_f32(vminq_f32(y, limit3), limitM3);
		const float32x4_t y2 = vmulq_f32(y, y);
		const float32x4_t num = vmulq_f32(y, vaddq_f32(c27, y2));
		const float32x4_t den = vmlaq_f32(den27, y2, den9);
		float32x4_t rd = vrecpeq_f32(den);
		rd = vmulq_f32(rd, vrecpsq_f32(den, rd));
		return vcvtq_n_s32_f32(vmulq_f32(num, rd), 31);
	};
	for (int32_t i = 0; i < padded; i += 8) {
		const float32x4_t a = vld1q_f32(x + i);
		const float32x4_t b = vld1q_f32(x + i + 4);
		const float32x4_t mx = vmaxq_f32(vabsq_f32(a), vabsq_f32(b));
		float32x2_t m = vmax_f32(vget_low_f32(mx), vget_high_f32(mx));
		m = vmul_n_f32(vpmax_f32(m, m), preGain);
		level = vmax_f32(level, m);
		// SLOPE(peak, m, attack, release) as release * e + (attack - release) * max(e, 0)
		const float32x2_t e = vsub_f32(m, peak);
		peak = vmla_f32(vmla_f32(peak, kRelease, e), kAttack, vmax_f32(e, zero));
		// Gain 1 up to a peak of 1, 1 / peak above
		float32x2_t r = vrecpe_f32(peak);
		r = vmul_f32(r, vrecps_f32(peak, r));
		r = vmul_f32(r, vrecps_f32(peak, r));
		const float32x2_t gain = vmul_f32(vmin_f32(r, one), scale);
		vst1q_s32(q + i, clip(vmulq_lane_f32(a, gain, 0)));
		vst1q_s32(q + i + 4, clip(vmulq_lane_f32(b, gain, 0)));
	}
	std::memcpy(out, q, n * sizeof(int32_t));
	v.limiterPeak = vget_lane_f32(peak, 0);
	return vget_lane_f32(level, 0) * 0.8f;
}

} // namespace

// API ----------------------------------------------------------------------------------------------------------------

uint32_t voiceSize() {
	return sizeof(Voice);
}

void reset(Voice& v) {
	v.rng = gSeed;
	gSeed = gSeed * 2891336453u + 0x9E3779B9u;
	v.chord = 0;
	v.holdoff = 0;
	v.levelValue = 0.f;
	v.lastBow = 0.f;
	v.pendingStrike = false;
	v.silent = true;
	for (float& k : v.modalKey) {
		k = 0.f;
	}
	std::memset(v.c1, 0, sizeof(v.c1));
	std::memset(v.a2, 0, sizeof(v.a2));
	std::memset(v.b0, 0, sizeof(v.b0));
	std::memset(v.e1, 0, sizeof(v.e1));
	std::memset(v.e2, 0, sizeof(v.e2));
	startModel(v, Model::MODAL);
}

void trigger(Voice& v) {
	v.pendingStrike = true;
}

void render(Voice& v, const Settings& settings, uint32_t basePhaseIncrement, int32_t* out, int32_t n) {
	n = std::clamp<int32_t>(n, 0, kMaxRender);
	if (n == 0) {
		return;
	}
	Settings s = settings;
	s.structure = std::clamp(s.structure, 0.f, 0.9995f);
	s.brightness = std::clamp(s.brightness, 0.f, 1.f);
	s.damping = std::clamp(s.damping, 0.f, 0.9995f);
	s.position = std::clamp(s.position, 0.f, 0.9995f);
	s.bow = std::clamp(s.bow, 0.f, 1.f);
	s.drive = std::clamp(s.drive, 0.f, 1.f);
	if (s.model >= Model::NUM_MODELS) {
		s.model = Model::MODAL;
	}
	if (s.model != v.model) {
		startModel(v, s.model);
	}
	v.lastBow = s.bow;
	const bool strike = v.pendingStrike;
	v.pendingStrike = false;
	if (strike) {
		v.holdoff = 4096;
		v.silent = false;
	}
	// Died away with nothing to wake it: nothing to work out
	if (v.silent && s.bow <= 0.f) {
		std::memset(out, 0, n * sizeof(int32_t));
		v.levelValue = 0.f;
		return;
	}
	v.silent = false;

	const float f0 = std::clamp(static_cast<float>(basePhaseIncrement) * (1.f / 4294967296.f), 1.f / kLineSize, 0.45f);
	alignas(16) float x[kMaxRender + 8];
	switch (s.model) {
	case Model::MODAL:
		renderModal(v, s, f0, strike, x, n);
		break;
	case Model::SYMPATHETIC:
	case Model::CHORD:
		renderSympathetic(v, s, f0, strike, x, n);
		break;
	case Model::STRING:
		renderString(v, s, f0, strike, x, n);
		break;
	default:
		renderFm(v, s, f0, strike, x, n);
		break;
	}
	const float peak = outputStage(v, x, out, n, kModelGain[static_cast<size_t>(s.model)], s.drive);

	// Level: the render's peak, falling by half each render without one
	v.levelValue = std::max(peak, v.levelValue * 0.5f);
	if (v.holdoff > 0) {
		v.holdoff -= n;
	}
	if (v.holdoff <= 0 && v.levelValue < 1e-6f && s.bow <= 0.f) {
		v.silent = true;
	}
}

float level(const Voice& v) {
	float l = v.levelValue;
	if (v.holdoff > 0 || v.pendingStrike) {
		l = std::max(l, 1.f);
	}
	if (v.lastBow > 0.f) {
		l = std::max(l, 1e-3f);
	}
	return l;
}

} // namespace deluge::dsp::drum_synth::rings
