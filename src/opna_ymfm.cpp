/* opna_ymfm.cpp - OPNA (YM2608) rendering via ymfm (third_party/ymfm).
 *   Mirrors the driving pattern proven in RetroDOS Player's renderer:
 *   OPN_FIDELITY_MED, prescaler writes (2Dh-2Fh) refresh the chip's
 *   native rate, and the chip-rate stream is box-filter averaged down
 *   to the host rate (44.1 kHz). Output mix is FM L/R plus the SSG
 *   output at 0.20 (at 1:1 the SSG measures 5-18 dB hot vs hardware).
 *   ADPCM-B gets a real 256 KiB RAM behind the chip; the ADPCM-A
 *   rhythm ROM reads as zero until a later phase supplies it.
 *   Audio is rendered only when P8PWAV names an output WAV file.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include "ymfm.h"
#include "ymfm_opn.h"
#include "opna.h"
#include "vessel.h"

namespace {

const uint32_t CHIP_CLOCK = 7987200;    /* OPNA master clock on SB2 */
const int HOST_RATE = 44100;
const long CPU_CLOCK = 4000000;         /* vessel CPU clock */

unsigned long g_ext_reads_b = 0, g_ext_writes_b = 0;   /* diagnostics */
unsigned long g_ext_reads_a = 0;

struct Iface : ymfm::ymfm_interface {
    std::vector<uint8_t> adpcm_b_ram;
    std::vector<uint8_t> adpcm_a_ram;   /* SB2's writable ADPCM-A memory */
    Iface() : adpcm_b_ram(256 * 1024, 0), adpcm_a_ram(1024 * 1024, 0) {}
    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t offset) override
    {
        if (type == ymfm::ACCESS_ADPCM_B && offset < adpcm_b_ram.size()) {
            g_ext_reads_b++;
            return adpcm_b_ram[offset];
        }
        if (type == ymfm::ACCESS_ADPCM_A && offset < adpcm_a_ram.size()) {
            g_ext_reads_a++;
            return adpcm_a_ram[offset];
        }
        return 0;
    }
    void ymfm_external_write(ymfm::access_class type, uint32_t offset, uint8_t data) override
    {
        if (type == ymfm::ACCESS_ADPCM_B && offset < adpcm_b_ram.size()) {
            g_ext_writes_b++;
            adpcm_b_ram[offset] = data;
        }
    }
};

Iface iface;
ymfm::ym2608 chip(iface);
ymfm::ym2608::output_data out;

int enabled = -1;               /* -1 unknown, 0 off, 1 on */
FILE *wav = nullptr;
uint32_t chip_rate = 0;
uint32_t total_frames = 0;
double res_pos = 0;             /* box-filter resampler position */
double last_l = 0, last_r = 0;
long long frame_acc = 0;        /* cycles * HOST_RATE not yet rendered */
int16_t outbuf[4096 * 2];
int outfill = 0;
int trace_audio = 0;            /* P8P_TRACE_AUDIO: per-10 s render log */
int sec_peak = 0;

void wav_header(uint32_t data_bytes)
{
    unsigned char h[44];
    memset(h, 0, sizeof(h));
    memcpy(h + 0, "RIFF", 4);
    auto put32 = [&](int off, uint32_t v) {
        h[off] = v & 0xFF; h[off + 1] = (v >> 8) & 0xFF;
        h[off + 2] = (v >> 16) & 0xFF; h[off + 3] = (v >> 24) & 0xFF;
    };
    auto put16 = [&](int off, uint16_t v) {
        h[off] = v & 0xFF; h[off + 1] = (v >> 8) & 0xFF;
    };
    put32(4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(16, 16);
    put16(20, 1);               /* PCM */
    put16(22, 2);               /* stereo */
    put32(24, HOST_RATE);
    put32(28, HOST_RATE * 4);
    put16(32, 4);
    put16(34, 16);
    memcpy(h + 36, "data", 4);
    put32(40, data_bytes);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fseek(wav, 0, SEEK_END);
}

void finalize(void)
{
    if (!wav) return;
    if (outfill) { fwrite(outbuf, 2, (size_t)outfill * 2, wav); outfill = 0; }
    wav_header(total_frames * 4);
    fclose(wav);
    wav = nullptr;
}

bool ensure_init(void)
{
    if (enabled >= 0) return enabled == 1;
    enabled = 0;
    const char *path = getenv("P8PWAV");
    if (!path || !*path) return false;
    wav = fopen(path, "wb");
    if (!wav) return false;
    chip.reset();
    chip.set_fidelity(ymfm::OPN_FIDELITY_MED);
    chip_rate = chip.sample_rate(CHIP_CLOCK);
    unsigned char zero[44] = {0};
    fwrite(zero, 1, 44, wav);   /* patched by finalize() */
    trace_audio = getenv("P8P_TRACE_AUDIO") != nullptr;
    atexit(finalize);
    enabled = 1;
    return true;
}

int16_t clamp16(double v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void push_frame(void)
{
    res_pos += (double)chip_rate / HOST_RATE;
    double al = 0, ar = 0;
    int n = 0;
    while (res_pos >= 1.0) {
        res_pos -= 1.0;
        chip.generate(&out, 1);
        al += out.data[0] + 0.20 * out.data[2];
        ar += out.data[1] + 0.20 * out.data[2];
        n++;
    }
    /* ymfm voices are full-amplitude each; the 6-channel digital sum
     * overflows int16 where the analog original would not, so the
     * harness WAV takes 0.5 of master headroom. */
    if (n) { last_l = al / n * 0.5; last_r = ar / n * 0.5; }
    outbuf[outfill * 2] = clamp16(last_l);
    outbuf[outfill * 2 + 1] = clamp16(last_r);
    if (trace_audio) {
        int p = outbuf[outfill * 2]; if (p < 0) p = -p;
        if (p > sec_peak) sec_peak = p;
        if ((total_frames + 1) % (HOST_RATE * 10) == 0) {
            fprintf(stderr, "AUDIO %u s: peak %d, keyons %lu, ssg %lu, pcma %lu, fm %lu, extA r %lu\n",
                    (total_frames + 1) / HOST_RATE, sec_peak,
                    vessel_opn_keyons(),
                    vessel_opn_class(0x00, 0x0F), vessel_opn_class(0x10, 0x1F),
                    vessel_opn_class(0x20, 0xFF), g_ext_reads_a);
            sec_peak = 0;
        }
    }
    outfill++;
    total_frames++;
    if (outfill == 4096) { fwrite(outbuf, 2, 4096 * 2, wav); outfill = 0; }
}

} // namespace

extern "C" void opna_adpcm_a_write(unsigned long addr, int value)
{
    if (addr < iface.adpcm_a_ram.size())
        iface.adpcm_a_ram[(size_t)addr] = (uint8_t)value;
}

extern "C" void opna_write(int bank, int addr, int value)
{
    if (!ensure_init()) return;
    if (bank == 0) {
        chip.write_address((uint8_t)addr);
        chip.write_data((uint8_t)value);
        if (addr >= 0x2D && addr <= 0x2F)
            chip_rate = chip.sample_rate(CHIP_CLOCK);
    } else {
        chip.write_address_hi((uint8_t)addr);
        chip.write_data_hi((uint8_t)value);
    }
}

extern "C" void opna_advance(long cpu_cycles)
{
    if (!ensure_init()) return;
    frame_acc += (long long)cpu_cycles * HOST_RATE;
    long frames = (long)(frame_acc / CPU_CLOCK);
    frame_acc %= CPU_CLOCK;
    if (frames > HOST_RATE) frames = HOST_RATE;   /* bound pathological deltas */
    for (long i = 0; i < frames; i++) push_frame();
}
