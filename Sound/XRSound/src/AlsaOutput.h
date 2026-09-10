// ==============================================================
// AlsaOutput.h — ALSA output for XRSound on Linux.
//
// See AlsaOutput.cpp for why this exists: irrKlang 1.6.0's own ALSA backend
// crackles on PipeWire systems in every configuration it offers, while the
// same files play cleanly through ALSA directly. irrKlang still does all the
// decoding and mixing; this takes over only the final write to the device,
// through the ISoundMixedOutputReceiver interface irrKlang publishes for
// exactly that purpose.
//
// Not compiled on Windows, which uses DirectSound8 and works.
// ==============================================================

#ifndef XRSOUND_ALSAOUTPUT_H
#define XRSOUND_ALSAOUTPUT_H

#ifndef _WIN32

#include <ik_ISoundMixedOutputReceiver.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// Opaque so <alsa/asoundlib.h> stays out of every translation unit that
// merely holds one of these.
typedef struct _snd_pcm snd_pcm_t;

// ==========================================================================
// THE CALLBACK MUST NOT BLOCK, AND THE FIRST VERSION OF THIS CLASS DID.
//
// OnAudioDataReady wrote straight to ALSA with a blocking snd_pcm_writei. The
// header comment even quoted the interface contract -- "return as fast as
// possible, otherwise sound output may be stuttering" -- next to the call
// that ignores it.
//
// The cost did not land on the audio. It landed on the SIMULATION, because
// irrKlang's multi-threaded engine holds its internal lock across the mix and
// the receiver call, so every XRSound call from Orbiter's thread queued
// behind a device write. MEASURED with per-phase frame accounting in
// "Earth views":
//
//   preStep 74.1 ms | plugins 74.1 (worst 'XRSound' 74.0)  vessels 0.0
//   update  70.7 ms | preStep 70.5  psys 0.1  dialogs 0.0  postStep 0.2
//
// -- 26 to 99 ms per frame inside XRSound's clbkPreStep, 7 to 11 times a
// second, while the entire Vulkan renderer measured 1-4 ms. That is the
// "frame'ish" stutter, and the buffer arithmetic predicts it exactly: a 0.5 s
// buffer in quarter-buffer periods is a 125 ms period, and writei blocks
// until a period drains.
//
// Removing XRSound from ACTIVE_MODULES took the same scenario from
// 7-11 frames per second over 33 ms to ZERO, at a flat 165 fps -- which is
// what identified the module before this file was ever opened.
//
// So the write moves to a thread of its own and the callback becomes a
// memcpy into a ring buffer. The blocking is still there, because a blocking
// write is how you pace an audio device; it just no longer happens on a
// thread anything else is waiting on.
//
// ==========================================================================
// AND THEN THE OPPOSITE FAULT, WHICH THAT CHANGE CREATED. MEASURED:
//
//   AlsaOutput: 41.9 s | in 1661883 B/s  out 177816 B/s  realtime 176400 B/s
//               (mixer x9.42, device x1.01)  dropped 352.3 s of audio
//   AlsaOutput: 0 underruns, 3601 overruns
//
// irrKlang's mixer was running at 9.42 TIMES REALTIME and 352 of the 396
// seconds of audio it produced in a 42-second session were thrown away. That
// is what "the sound is skipping" was.
//
// THE CAUSE IS THE DEVICE irrKlang WAS GIVEN. XRSoundEngine.cpp opens it on
// ALSA's own "null" PCM so its broken output goes nowhere -- and alsa-lib's
// null plugin accepts every write instantly and reports its whole buffer
// free, for ever. A sound device is what paces a mixing loop; a device that
// never blocks paces nothing. With the receiver returning immediately as
// well, NOTHING in the chain was rate-limiting, and the mixer ran as fast as
// the CPU allowed. The old blocking write had been the accidental pacer, and
// removing it removed the pacing with it.
//
// So the callback WAITS FOR ROOM, and that is not a return to the old
// behaviour. Three things make it different:
//
//   * It waits on the RING, not on the device, so what it waits for is the
//     writer thread freeing space -- and the writer frees space in whole
//     dequeues, without ever holding the lock across a device write.
//   * It waits against a TARGET FILL (kTargetMs), not against the ring being
//     full, so latency is the target and not the buffer size.
//   * The wait is BOUNDED, and a timeout falls through to the old
//     drop-the-oldest path, so a wedged or vanished device degrades to the
//     previous behaviour instead of hanging irrKlang's thread for ever.
//
// The stall this puts back on the mixing thread -- and so, through irrKlang's
// engine lock, on Orbiter's simulation thread -- is bounded by how often the
// writer frees ring space, which is one ALSA period. That is why Open no
// longer asks for a half-second buffer: at 0.5 s ALSA uses 125 ms periods and
// the stall would be the very hitch this file was written to remove. See the
// latency note in Open.
// ==========================================================================
class AlsaOutput : public irrklang::ISoundMixedOutputReceiver
{
public:
    AlsaOutput() = default;
    virtual ~AlsaOutput();

    // ISoundMixedOutputReceiver. Called from irrKlang's mixing thread with
    // 16-bit stereo interleaved frames; returns without touching the device.
    virtual void OnAudioDataReady(const void *data, int byteCount,
                                  int playbackrate) override;

    // MEASURE-ONLY: count and report, write nothing, open nothing.
    //
    // The default configuration lets irrKlang do its own output on a real
    // PCM (see XRSoundEngine.cpp), and a receiver that also opened the
    // device would put a second stream on the same sink -- which is what
    // dragged the measured mixer rate from 1.00 down to 0.74 while it was
    // being investigated. In this mode the class is purely an instrument:
    // the rate, the peak and the chunk size still come out at teardown and
    // nothing it does can affect the audio.
    void SetMeasureOnly(bool b) { m_measure = b; }

    // Diagnostics, for the XRSound log.
    const char   *LastError()  const { return m_lastError; }
    unsigned long Underruns()  const { return m_underruns; }
    unsigned long Overruns()   const { return m_overruns; }
    bool          IsOpen()     const { return m_pcm != nullptr; }

private:
    bool Open(int playbackRate);
    void Close();
    void WriterLoop();          // the only function that touches m_pcm's data path

    snd_pcm_t    *m_pcm  = nullptr;
    bool          m_measure = false;   // instrument only; no device, no writer
    int           m_rate = 0;
    unsigned long m_underruns = 0;
    unsigned long m_overruns  = 0;   // ring full: the mixer outran the device
    char          m_lastError[256] = {0};

    // ==================================================================
    // THE PRODUCTION RATE, because "overruns" alone does not say whether
    // the mixer is a little ahead or a hundred times ahead.
    //
    // A realtime stereo 16-bit stream is exactly rate*4 bytes per second.
    // Comparing what OnAudioDataReady delivered against that number is the
    // one measurement that separates "the ring is slightly too small" from
    // "the producer is not paced at all", and those need opposite fixes.
    //
    // Both are written under m_mx by the thread that owns the transfer, so
    // no atomics are needed: the mixer adds to bytesIn while it holds the
    // lock to copy in, the writer adds to bytesOut while it holds the lock
    // to copy out.
    // ==================================================================
    unsigned long long m_bytesIn  = 0;   // delivered by irrKlang's mixer
    unsigned long long m_bytesOut = 0;   // taken out of the ring for the device
    unsigned long long m_bytesDropped = 0;
    double             m_t0 = 0.0;       // seconds, monotonic, at first delivery
    // AND THE LAST, because the window has to be the one the mixer was
    // actually running in. Measuring to teardown instead folds the gap
    // between the engine being dropped and this object being deleted into
    // the divisor, which drags the ratio below 1.0 and makes a healthy
    // stream look like a starved one.
    double             m_tLast = 0.0;
    size_t             m_chunkIn = 0;    // irrKlang's own delivery size, once seen
    unsigned long      m_paceWaits = 0;  // callbacks that had to wait for room
    double             m_paceWorst = 0.0;// longest single wait, seconds

    // THE PEAK SAMPLE, and it is the only thing in this file that can say
    // whether any sound was produced at all.
    //
    // Every other counter here measures BYTES, and a stream of silence is
    // exactly as many bytes as a stream of engine noise. The porting notes carried "Sound audibility -- nobody has
    // confirmed a sound was heard" as an open item for that reason: nothing
    // in this process can listen. A peak amplitude cannot hear either, but it
    // can distinguish silence from not-silence, which is most of the
    // question and costs one pass over a buffer that was just memcpy'd.
    int                m_peak = 0;       // max |sample| over the session

    // ==================================================================
    // THE TARGET FILL, and it is the pacing threshold -- not the ring size.
    //
    // OnAudioDataReady waits while the ring holds this much or more, which
    // is what throttles irrKlang's mixer to realtime. Pacing on "the ring is
    // FULL" instead would work too, but then the ring's size would BE the
    // latency, and the ring also has to be big enough to never truncate one
    // of irrKlang's deliveries. Separating the two lets the ring stay
    // generous while the latency stays short.
    //
    // Set from the rate at Open: bytes for kTargetMs of stereo 16-bit audio.
    // ==================================================================
    static const unsigned kTargetMs = 100;
    size_t             m_targetBytes = 0;

    // The ring. One second at 48 kHz stereo 16-bit is 192,000 bytes; 256 KB
    // gives that with headroom and keeps the arithmetic in whole bytes rather
    // than frames, because every producer and consumer here works in bytes.
    //
    // Sized to absorb a scheduling hiccup, NOT to add latency: the writer
    // drains it as fast as the device accepts, so in steady state it holds
    // roughly one period.
    static const size_t kRingBytes = 256 * 1024;
    std::vector<char>       m_ring;
    size_t                  m_head = 0;      // write position (mixer thread)
    size_t                  m_tail = 0;      // read position  (writer thread)
    size_t                  m_used = 0;
    std::mutex              m_mx;
    std::condition_variable m_cv;
    std::thread             m_writer;
    std::atomic<bool>       m_stop{false};
};

#endif // !_WIN32
#endif // XRSOUND_ALSAOUTPUT_H
