// ==============================================================
// AlsaOutput.cpp — ALSA output for XRSound on Linux.
//
// irrKlang 1.6.0's own ALSA backend does not work on a modern PipeWire
// system. Measured, not assumed: the same 48 kHz and 44.1 kHz files play
// cleanly through `aplay` on the pipewire-alsa PCM, and crackle through
// irrKlang in every configuration it offers -- ESOD_AUTO_DETECT with no
// device, ESOD_ALSA with "default", and ESOD_ALSA with "plug:default", single
// threaded and multi threaded alike. Six combinations, all bad, with Orbiter
// entirely out of the picture. The library dates from 2018 and predates
// PipeWire.
//
// What is broken is only the final step, where irrKlang writes PCM to the
// device. Its decoding and mixing are fine. irrKlang provides a documented
// way to take that step over, stated in ik_ESoundOutputDrivers.h for the ALSA
// driver: "Supports the ISoundMixedOutputReceiver interface using
// setMixedDataOutputReceiver."
//
// So irrKlang mixes, hands the finished buffer here, and this writes it to
// ALSA the way aplay does. That is an implementation of a published interface
// replacing a broken backend, not a workaround layered over it.
//
// The Windows build never compiles this file: it uses DirectSound8, which
// works, and XRSoundEngine.cpp keeps its original createIrrKlangDevice call
// verbatim under #ifdef _WIN32.
// ==============================================================

#ifndef _WIN32

#include "AlsaOutput.h"
#include <alsa/asoundlib.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <chrono>

// A monotonic clock in seconds. CLOCK_MONOTONIC and not the wall clock: the
// whole point of the numbers it stamps is elapsed time, and a wall clock can
// step.
static double NowSec()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

AlsaOutput::~AlsaOutput()
{
    Close();
}

// Open the device for the rate irrKlang tells us it is mixing at.
//
// The format is fixed by the interface contract in
// ik_ISoundMixedOutputReceiver.h: "Sound data always consists of two
// interleaved sound channels at 16bit per frame." So S16_LE, 2 channels, and
// only the rate is variable.
bool AlsaOutput::Open(int playbackRate)
{
    Close();

    // MEASURE-ONLY stops here: no PCM, no ring, no writer thread. m_rate is
    // still set because it is the divisor every reported ratio uses.
    if (m_measure) {
        m_rate = playbackRate;
        m_targetBytes = 0;
        m_lastError[0] = '\0';
        return true;
    }

    // "default" is the PCM the pipewire-alsa plugin owns, and is what aplay
    // uses. SND_PCM_NONBLOCK is deliberately NOT set: OnAudioDataReady is
    // called from irrKlang's mixing thread, which is exactly the thread that
    // should block while the device drains.
    int err = snd_pcm_open(&m_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        snprintf(m_lastError, sizeof(m_lastError),
                 "snd_pcm_open failed: %s", snd_strerror(err));
        m_pcm = nullptr;
        return false;
    }

    // ==================================================================
    // THE BUFFER LENGTH IS THE SIMULATION'S HITCH BUDGET, not just latency.
    //
    // It was 500000 (0.5 s) to match what the plugin negotiates for aplay,
    // on the reasoning that "small periods are what starve this path". That
    // reasoning belonged to the version that wrote to the device from
    // irrKlang's own thread; with a writer thread of its own, a short period
    // costs nothing and a long one is actively harmful.
    //
    // snd_pcm_set_params uses quarter-buffer periods, and snd_pcm_writei
    // returns when a period's worth of space appears. That period is
    // therefore how often the writer frees ring space, and so how long
    // OnAudioDataReady can be kept waiting for room -- and irrKlang holds its
    // engine lock across that callback, so it is also how long Orbiter's
    // simulation thread can be kept waiting for XRSound. At 0.5 s that is a
    // 125 ms stall, which is precisely the fault this file exists to remove.
    //
    // 80 ms of buffer gives 20 ms periods: four periods of cushion against a
    // late writer thread (0 underruns measured), and a bounded stall short
    // enough not to cost a frame at 165 fps. ORB_ALSA_LATENCY_US overrides it
    // for tuning without a rebuild -- underruns and the worst pacing wait are
    // both reported at teardown, so the trade is measurable in one run.
    // ==================================================================
    unsigned int latency_us = 80000;          // 0.08 s -> 20 ms periods
    if (const char *env = getenv("ORB_ALSA_LATENCY_US")) {
        const long v = atol(env);
        if (v >= 10000 && v <= 1000000) latency_us = (unsigned int)v;
    }

    err = snd_pcm_set_params(m_pcm,
                             SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             2,                 // stereo, per the contract
                             (unsigned)playbackRate,
                             1,                 // allow the plugin to resample
                             latency_us);
    if (err < 0) {
        snprintf(m_lastError, sizeof(m_lastError),
                 "snd_pcm_set_params failed: %s", snd_strerror(err));
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }

    m_rate = playbackRate;
    m_lastError[0] = '\0';

    // THE PACING THRESHOLD, in bytes of this stream. kTargetMs of stereo
    // 16-bit audio, rounded down to a whole frame so the ring never holds a
    // fraction of one. Clamped to leave the ring room for two of irrKlang's
    // deliveries above the target, because the callback copies AFTER the
    // wait: if the target were the ring size the copy would have nowhere to
    // go and would fall through to dropping every time.
    m_targetBytes = (size_t)((double)playbackRate * 4.0 * (kTargetMs / 1000.0));
    m_targetBytes -= m_targetBytes % 4;
    if (m_targetBytes > kRingBytes / 2) m_targetBytes = kRingBytes / 2;
    if (m_targetBytes < 4096)           m_targetBytes = 4096;

    // The ring and its writer, once the device is known to be good. Allocated
    // here rather than in the constructor so a failed Open costs nothing, and
    // because kRingBytes is fixed this never reallocates under the mixer.
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_ring.assign(kRingBytes, 0);
        m_head = m_tail = m_used = 0;
    }
    m_stop  = false;
    m_writer = std::thread(&AlsaOutput::WriterLoop, this);

    return true;
}

void AlsaOutput::Close()
{
    // Stop the writer FIRST. It is the only thread that touches m_pcm's data
    // path, so closing the device under it would be a use-after-free of the
    // handle rather than a tidy shutdown.
    if (m_writer.joinable()) {
        m_stop = true;
        m_cv.notify_all();
        m_writer.join();
    }
    m_stop = false;

    // THE REPORT IS GATED ON m_rate, NOT ON m_pcm, because measure-only mode
    // never opens a device and its numbers are the whole reason it exists.
    if (m_rate) {
        // The two numbers that say whether the ring is sized right, printed
        // once at teardown rather than never. Underruns mean the device ran
        // dry (the writer was late); OVERRUNS mean irrKlang outran the device
        // and audio was dropped, which is the failure mode this design trades
        // for -- it is bounded and audible, where blocking was unbounded and
        // stalled the simulation.
        // AND THE PRODUCTION RATE, which is the number that says WHY.
        //
        // A realtime stereo 16-bit stream is rate*4 bytes per second. If
        // "in" is close to that, the mixer is paced and any overruns are a
        // sizing problem; if it is a multiple of it, the mixer is not paced
        // at all and no ring size can help. Those need opposite fixes and
        // the overrun count alone cannot tell them apart.
        {
            const double dt = (m_t0 > 0.0 && m_tLast > m_t0) ? (m_tLast - m_t0) : 0.0;
            const double realtime = (double)m_rate * 4.0;
            if (dt > 0.5 && realtime > 0.0) {
                fprintf(stderr,
                        "AlsaOutput: %.1f s | in %.0f B/s  out %.0f B/s  "
                        "realtime %.0f B/s  (mixer x%.2f, device x%.2f)  "
                        "dropped %.1f s of audio\n",
                        dt, (double)m_bytesIn / dt, (double)m_bytesOut / dt,
                        realtime,
                        ((double)m_bytesIn / dt) / realtime,
                        ((double)m_bytesOut / dt) / realtime,
                        (double)m_bytesDropped / realtime);
            }
        }

        // AND THE PACING, which is what makes the ratio above come out at 1.
        // paceWaits counts the callbacks that had to wait for room -- in a
        // healthy session that is nearly all of them, because the mixer is
        // faster than realtime and being held back is the point. WORST is
        // the number that matters: it is the longest the mixer thread was
        // held, and so the longest Orbiter's simulation thread could have
        // been held behind irrKlang's engine lock.
        fprintf(stderr,
                "AlsaOutput: pacing: target %zu B (%u ms), mixer chunk %zu B, "
                "%lu waits, worst %.1f ms; peak sample %d/32767 (%s)\n",
                m_targetBytes, kTargetMs, m_chunkIn, m_paceWaits,
                m_paceWorst * 1000.0, m_peak,
                m_peak == 0 ? "SILENCE -- nothing was audible"
                            : "audio present");

        if (m_underruns || m_overruns)
            fprintf(stderr, "AlsaOutput: %lu underruns, %lu overruns\n",
                    m_underruns, m_overruns);

    }

    if (m_pcm) {
        snd_pcm_drain(m_pcm);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
    }
    m_rate = 0;

    std::lock_guard<std::mutex> lk(m_mx);
    m_head = m_tail = m_used = 0;
}

// The device's own thread. Everything here is allowed to block; nothing else
// waits on it.
void AlsaOutput::WriterLoop()
{
    // A chunk, reused. Sized at the PACING TARGET rather than at a quarter of
    // the ring, and that is not a cosmetic change: the writer takes at most
    // one chunk per pass, and one pass is one snd_pcm_writei, so the chunk is
    // how much audio the device is asked to swallow in a single blocking
    // call. At a quarter of a 256 KB ring that was 64 KB -- 372 ms at 44.1
    // kHz -- and ring space was therefore freed in 372 ms lumps, which is
    // exactly the granularity the producer would then have to wait at.
    //
    // At the target it is one target's worth (100 ms), which is also all the
    // ring should ever hold, so nothing is lost by not asking for more.
    std::vector<char> chunk(m_targetBytes ? m_targetBytes : (kRingBytes / 8));

    for (;;) {
        size_t n = 0;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            m_cv.wait(lk, [this] { return m_used > 0 || m_stop.load(); });
            if (m_stop && m_used == 0) return;

            n = m_used < chunk.size() ? m_used : chunk.size();

            // Out of the ring, honouring the wrap.
            const size_t first = (m_tail + n <= kRingBytes) ? n : (kRingBytes - m_tail);
            memcpy(chunk.data(), m_ring.data() + m_tail, first);
            if (first < n) memcpy(chunk.data() + first, m_ring.data(), n - first);
            m_tail = (m_tail + n) % kRingBytes;
            m_used -= n;
            m_bytesOut += n;
        }

        // ROOM. The producer is waiting on this same condition variable for
        // the fill to drop below the target, and the dequeue above is the
        // only thing that ever lowers it. notify_all and not notify_one:
        // there are two different waiters on this variable now -- this loop
        // waiting for data and the mixer waiting for room -- and waking the
        // wrong one would leave the other asleep with its predicate true.
        m_cv.notify_all();

        // ORB_ALSA_NOWRITE: drain the ring but write nothing to the device.
        //
        // THE CONTROL FOR "IS irrKlang'S OWN OUTPUT ANY GOOD". With a real
        // ORB_IRRKLANG_DEVICE, irrKlang opens the speakers and so do we, and
        // two streams on one sink is not a measurement of either. With this
        // set there is exactly one stream -- irrKlang's -- and the receiver
        // is reduced to an instrument: the in-rate and the peak still say
        // what irrKlang produced, and nothing this class does can affect it.
        static const bool bNoWrite = (getenv("ORB_ALSA_NOWRITE") != nullptr);
        if (bNoWrite) continue;

        // OUTSIDE THE LOCK. This is the call that blocks for up to a period,
        // and the whole point of this class is that nothing else is waiting
        // behind it.
        snd_pcm_uframes_t frames = (snd_pcm_uframes_t)(n / 4);
        const char *p = chunk.data();

        while (frames > 0 && m_pcm) {
            snd_pcm_sframes_t written = snd_pcm_writei(m_pcm, p, frames);

            if (written == -EPIPE) {
                // Underrun. Recover and carry on rather than tearing the
                // device down: a dropped chunk is a click, a lost device is
                // silence.
                snd_pcm_prepare(m_pcm);
                ++m_underruns;
                continue;
            }
            if (written == -ESTRPIPE) {
                // The device was suspended (system sleep). Wait for it.
                int err;
                while ((err = snd_pcm_resume(m_pcm)) == -EAGAIN)
                    snd_pcm_wait(m_pcm, 100);
                if (err < 0) snd_pcm_prepare(m_pcm);
                continue;
            }
            if (written < 0) {
                // Anything else is not recoverable for this chunk. Drop it;
                // the stream resumes with the next one.
                snd_pcm_prepare(m_pcm);
                break;
            }

            p      += written * 4;
            frames -= written;
        }
    }
}

// Called from irrKlang's mixing thread. The interface asks implementations to
// "return as fast as possible, otherwise sound output may be stuttering", so
// there is no allocation and no logging on the normal path.
void AlsaOutput::OnAudioDataReady(const void *data, int byteCount, int playbackrate)
{
    if (!data || byteCount <= 0) return;

    // The rate is documented as fixed for the lifetime of an engine, but the
    // device is opened lazily here rather than at construction so the real
    // value is used instead of an assumed one.
    // m_measure is part of the test because measure-only never sets m_pcm:
    // without it, every single callback would re-Open and the rate would be
    // reset before it could ever be reported.
    if ((!m_pcm && !m_measure) || playbackrate != m_rate) {
        if (!Open(playbackrate))
            return;         // Open recorded why; nothing further to be done
    }

    // A MEMCPY AND A NOTIFY. Nothing here touches the device.
    //
    // irrKlang's engine holds its internal lock across the mix and this call,
    // so any millisecond spent here is a millisecond Orbiter's simulation
    // thread spends waiting the next time it asks XRSound for anything. See
    // the note in AlsaOutput.h for what that cost measured.
    const size_t n = (size_t)byteCount;

    std::unique_lock<std::mutex> lk(m_mx);

    const double tnow = NowSec();
    if (m_t0 == 0.0) m_t0 = tnow;
    m_tLast = tnow;
    if (!m_chunkIn) m_chunkIn = n;
    m_bytesIn += n;

    // THE PEAK, over the buffer irrKlang just handed us. One pass over data
    // that is about to be memcpy'd anyway, so it is already in cache; at ten
    // deliveries a second this is a few microseconds of the callback.
    {
        const short *s = (const short *)data;
        const size_t ns = n / 2;
        for (size_t i = 0; i < ns; ++i) {
            int v = s[i]; if (v < 0) v = -v;
            if (v > m_peak) m_peak = v;
        }
    }

    // MEASURE-ONLY ENDS HERE. Everything below moves audio; in this mode
    // irrKlang is doing its own output on a real PCM and this class must
    // touch nothing. Note the counters above have already run, so the rate
    // and the peak still come out -- which is the point of the mode.
    if (m_measure) return;

    // ==================================================================
    // WAIT FOR ROOM. THIS IS THE PACING, and without it nothing in the
    // chain is rate-limited at all.
    //
    // irrKlang's device is ALSA's "null" PCM (see XRSoundEngine.cpp), which
    // accepts every write instantly and never blocks. A mixing loop is paced
    // by its device; a device that never blocks paces nothing. With this
    // callback also returning immediately, the mixer ran at 9.42 times
    // realtime and 89% of what it produced was discarded here -- which is
    // what the sound skipping was. The device is the ONLY realtime clock in
    // this pipeline, and this wait is how the mixer is tied to it.
    //
    // The wait is against m_targetBytes, not against the ring being full, so
    // it is the LATENCY that is bounded at kTargetMs rather than at the ring
    // size. It is also bounded in TIME: kWaitMs is several ALSA periods, so
    // a healthy device always frees space long before it expires, and a
    // wedged or unplugged one falls through to the drop path below instead
    // of hanging irrKlang's thread for ever.
    // ==================================================================
    // ORB_ALSA_NOPACE turns the wait off, which is the CONTROL for the
    // measurement above: with it set, whatever rate the mixer runs at is the
    // rate its own device paces it to, and nothing else. It is a diagnostic,
    // not a setting -- with the null device it restores the 9.42x fault.
    static const bool bNoPace = (getenv("ORB_ALSA_NOPACE") != nullptr);

    if (!bNoPace && m_used >= m_targetBytes && !m_stop.load()) {
        const double tw = NowSec();
        const auto kWaitMs = std::chrono::milliseconds(250);
        m_cv.wait_for(lk, kWaitMs,
                      [this] { return m_used < m_targetBytes || m_stop.load(); });
        const double waited = NowSec() - tw;
        ++m_paceWaits;
        if (waited > m_paceWorst) m_paceWorst = waited;
    }

    // Still no room means the device is not draining -- gone, wedged, or
    // slower than realtime. The response is then the one this class was
    // built with: DROP THE OLDEST audio rather than wait any longer, because
    // dropping the newest would stutter while leaving stale audio queued
    // ahead of it. With the wait above in place this is a fault path, and
    // the overrun counter is what says whether it is being taken.
    if (m_used + n > kRingBytes) {
        const size_t drop = (m_used + n) - kRingBytes;
        m_tail = (m_tail + drop) % kRingBytes;
        m_used -= drop;
        m_bytesDropped += drop;
        ++m_overruns;
    }

    const char *p = (const char *)data;
    const size_t first = (m_head + n <= kRingBytes) ? n : (kRingBytes - m_head);
    memcpy(m_ring.data() + m_head, p, first);
    if (first < n) memcpy(m_ring.data(), p + first, n - first);
    m_head = (m_head + n) % kRingBytes;
    m_used += n;

    m_cv.notify_one();
}

#endif // !_WIN32
