#include "wake_word_task.h"
#include "wake_word.h"
#include "audio_in.h"
#include "pir.h"
#include "ld2410c.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

static const char *TAG = "wake_word_task";

// 10 ms of 16 kHz int16 = 160 samples = exactly one feature hop.
#define CHUNK_SAMPLES  160

// Fallback self-mute in case main.c forgets to pause/resume us around
// voice_turn — a full voice_turn can run ~20 s, so give ample headroom.
#define POST_DETECT_MUTE_MS  25000

static EventGroupHandle_t s_trigger_events = NULL;
static EventBits_t        s_trigger_bit    = 0;
static atomic_bool        s_paused         = false;
static atomic_uint        s_last_score     = 0;

// After a reset, swallow this many inference steps before honoring detections.
// The streaming model can emit transient high scores for the first ~30 ms as
// its VAR_HANDLE state rebuilds from zero; we want that to settle first.
// 20 steps * 10 ms/step = 200 ms blind window.
#define WARMUP_STEPS_AFTER_RESET 20
static atomic_uint s_warmup_left = 0;

// Require N consecutive inference steps scoring above threshold before firing.
// A real "Yo Demerzel" peak sustains >= 3 steps (90+ ms at the model's stride-3
// cadence); conversational false fires are typically a single-frame phonetic
// coincidence. This is the primary gate against the 75% FP rate seen at
// threshold 220 during open conversation (Session 55 user report). If FPs
// persist at 2, bump to 3. If real wakes get missed, lower threshold instead.
#define WAKE_CONSECUTIVE_STEPS 2

// Saturation gate: reject detections when the recent audio window shows
// heavy clipping (INMP441 samples at ±32768). Loud conversation FPs
// observed in Session 55 consistently showed `pcm_peak=32768` in the
// heartbeat windows where the model fired at score=243-255 on sentences
// like "Well that's weird having air" — the model is overconfident on loud
// speech regardless of phonetic content, likely a training-dataset volume
// imbalance. Real "Yo Demerzel" spoken at normal room volume (1-2 m from
// the unit) typically clips <1% of samples. 5% is a clean boundary.
// Ring size is 1.5 s because the pre-window (below) needs to look at audio
// 1000-1500 ms BEFORE the wake detection. "Yo Demerzel" is ~800 ms long, so
// anything more recent than 1000 ms ago overlaps the user's own utterance and
// inflates pre-window energy — which is how Session 55 was rejecting legit
// loud wakes (pre-window 500-1000 ms ago was capturing "Yo De-" as if it
// were preceding speech).
#define SAT_WINDOW_CHUNKS 150

// Saturation gate is DISABLED (kept in heartbeat log as sat_pct for observability
// only). It couldn't distinguish legit loud wakes (17-27% clipped when spoken
// emphatically or close to the mic) from loud-speech FPs. The pre-gate is the
// sole acoustic filter now.
#define SAT_RECENT_CHUNKS 30
#define SAT_REJECT_PCT    101         // > 100 → never fires (all percentages are ≤100)

// Quiet-before gate: require the audio window 1000-1500 ms BEFORE the wake
// detection to have been quiet (low pcm_meansq). 1000 ms offset ensures the
// user's own ~800 ms "Yo Demerzel" utterance is strictly AFTER this window,
// so only pre-existing ambient is evaluated.
//
// Threshold 100M: idle baseline is 2-5M, briefly-after-speech decays through
// 10-80M (the tail takes ~1 s to fully settle after loud dictation), actively
// speaking rooms sit at 300M+. 100M admits "user paused ~1 s, dictation tail
// still decaying" without admitting continuous or near-continuous speech.
// Tuned 2026-04-23 (Session 55) from 30M after field reports that 1 s pauses
// weren't sufficient — rejected attempts were showing 60-77M pre_meansq.
#define QUIET_PRE_OFFSET  100         // start 1000 ms before fire
#define QUIET_PRE_SPAN    50          // span 500 ms (i.e., 1000-1500 ms pre-fire)
#define QUIET_MAX_MEANSQ  100000000U  // avg meansq in pre-window must be below this

// Presence holdoff: after any positive presence signal, consider the room
// occupied for this long even if both sensors go quiet (handles brief
// LD2410C dropout when a person is very still).
#define PRESENCE_HOLDOFF_US  (30LL * 1000000LL)  // 30 seconds

static bool check_room_occupied(int64_t *last_presence_us)
{
    bool pir = pir_get_state();

    ld2410c_state_t ld;
    bool radar = false;
    if (ld2410c_get_state(&ld) == ESP_OK) {
        radar = ld.presence;
    }

    int64_t now = esp_timer_get_time();
    if (pir || radar) {
        *last_presence_us = now;
        return true;
    }
    return (now - *last_presence_us) < PRESENCE_HOLDOFF_US;
}

static void wake_word_task_fn(void *arg)
{
    int16_t pcm[CHUNK_SAMPLES];

    esp_err_t r = wake_word_init(WAKE_WORD_FRONTEND_TFLM);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "wake_word_init failed: %s — task exiting", esp_err_to_name(r));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "wake-word task running (chunk=%d samples, threshold=%u)",
             CHUNK_SAMPLES, wake_word_get_threshold());

    uint8_t  window_max     = 0;
    uint32_t window_steps   = 0;
    int32_t  window_pcm_peak = 0;
    int64_t  window_pcm_ms   = 0;     // sum of squares
    uint32_t window_samples  = 0;
    uint32_t window_rejected_spikes = 0;
    uint32_t window_saturation_rejects = 0;
    uint32_t window_noisy_pre_rejects = 0;
    int64_t  last_log_us    = esp_timer_get_time();
    int64_t  last_presence_us = esp_timer_get_time();  // assume occupied at boot
    bool     was_occupied    = true;

    // Consecutive-step tracker for the spike gate. Only advanced when inference
    // actually ran (steps > 0), because g_last_score doesn't update on the
    // intermediate task iterations between stride-3 inferences.
    uint32_t consec_above = 0;

    // Rolling per-chunk data (1 s = 100 chunks). sat_ring[i] = clipped-sample
    // count in chunk i (capped at CHUNK_SAMPLES so uint8_t suffices, since
    // CHUNK_SAMPLES is 160 < 255). meansq_ring[i] = pcm mean-square of chunk i
    // (for the quiet-before gate). Both rings share the same index.
    // sat_pct is computed on demand from the NEWEST SAT_RECENT_CHUNKS entries.
    uint8_t  sat_ring[SAT_WINDOW_CHUNKS] = {0};
    uint32_t meansq_ring[SAT_WINDOW_CHUNKS] = {0};
    int      sat_ring_idx = 0;

    while (1) {
        size_t got = audio_in_consume_wake(pcm, CHUNK_SAMPLES, portMAX_DELAY);
        if (got == 0) continue;

        // Presence gate: skip inference when nobody's in the room.
        bool occupied = check_room_occupied(&last_presence_us);
        if (occupied != was_occupied) {
            ESP_LOGI(TAG, "room %s", occupied ? "OCCUPIED — wake-word active" : "EMPTY — inference suspended");
            if (occupied) {
                wake_word_reset();
                atomic_store(&s_warmup_left, WARMUP_STEPS_AFTER_RESET);
            }
            was_occupied = occupied;
        }
        if (!occupied) continue;  // drain samples, skip inference

        // Track PCM peak + RMS for this heartbeat window, plus clipped-sample
        // count for the saturation gate's rolling 1 s window and per-chunk
        // meansq for the quiet-before gate.
        uint16_t chunk_clipped = 0;
        int64_t  chunk_sum_sq  = 0;
        for (size_t i = 0; i < got; ++i) {
            int32_t s = pcm[i];
            int32_t a = s < 0 ? -s : s;
            if (a > window_pcm_peak) window_pcm_peak = a;
            chunk_sum_sq += (int64_t)s * (int64_t)s;
            if (a >= 32767) chunk_clipped++;  // ±32767 and -32768 both count
        }
        window_pcm_ms += chunk_sum_sq;
        window_samples += (uint32_t)got;

        uint32_t chunk_meansq =
            got ? (uint32_t)(chunk_sum_sq / (int64_t)got) : 0;

        // Advance the rings: overwrite oldest slot, move index forward.
        sat_ring[sat_ring_idx] =
            (uint8_t)(chunk_clipped > 255 ? 255 : chunk_clipped);
        meansq_ring[sat_ring_idx] = chunk_meansq;
        sat_ring_idx = (sat_ring_idx + 1) % SAT_WINDOW_CHUNKS;

        if (atomic_load(&s_paused)) {
            // While paused, drain samples but DO NOT feed the model — we don't
            // want TTS playback / our own voice to propagate into the streaming
            // state and produce a spurious detection immediately after resume.
            continue;
        }

        size_t steps = 0;
        int score = wake_word_feed(pcm, got, &steps);
        atomic_store(&s_last_score, (unsigned)score);

        // Drain warmup credits for any inference steps that just fired.
        // The streaming model can emit transient spikes for the first few
        // steps after a state reset; we ignore detections during that window.
        // Also clear the consecutive-step counter — we don't want a partial
        // streak from before the reset / pause to carry into the post-warmup
        // first frame and cause a premature fire.
        unsigned warm = atomic_load(&s_warmup_left);
        if (warm > 0) {
            unsigned consume = (unsigned)steps;
            if (consume > warm) consume = warm;
            atomic_store(&s_warmup_left, warm - consume);
            consec_above = 0;
            continue;
        }

        if ((uint8_t)score > window_max) window_max = (uint8_t)score;
        window_steps += (uint32_t)steps;

        // Consecutive-above-threshold gate. Only track when inference actually
        // ran this iteration — between stride-3 inferences the score stays
        // pinned at the previous result and wake_word_detected() would return
        // the same answer without new evidence.
        bool fire = false;
        if (steps > 0) {
            uint8_t thr = wake_word_get_threshold();
            if ((uint8_t)score >= thr) {
                if (consec_above < UINT32_MAX) consec_above++;
                if (consec_above >= WAKE_CONSECUTIVE_STEPS) {
                    fire = true;
                }
            } else {
                // Streak broken. If it was non-zero but under the gate, log it
                // so we can see the FP-filtering in action.
                if (consec_above > 0 && consec_above < WAKE_CONSECUTIVE_STEPS) {
                    window_rejected_spikes++;
                }
                consec_above = 0;
            }
        }

        // Saturation percentage over the newest SAT_RECENT_CHUNKS (300 ms).
        // Sampling the tail of the ring — the wake utterance itself occupies
        // roughly the newest 300-500 ms, so this captures clipping in the
        // utterance without dragging in stale clipping from pre-wake audio
        // (which the earlier 1 s window was doing, killing legit wakes
        // spoken after a loud conversation had just ended).
        uint32_t sat_recent_clips = 0;
        for (int k = 1; k <= SAT_RECENT_CHUNKS; ++k) {
            int idx = ((int)sat_ring_idx - k + SAT_WINDOW_CHUNKS) % SAT_WINDOW_CHUNKS;
            sat_recent_clips += sat_ring[idx];
        }
        uint32_t sat_pct =
            (sat_recent_clips * 100U) /
            ((uint32_t)SAT_RECENT_CHUNKS * (uint32_t)CHUNK_SAMPLES);

        int64_t now = esp_timer_get_time();
        if (now - last_log_us > 5000000LL) {  // every 5 s
            int32_t rms = window_samples ? (int32_t)(window_pcm_ms / (int64_t)window_samples) : 0;
            ESP_LOGI(TAG, "heartbeat: score_max=%u steps=%lu pcm_peak=%ld pcm_meansq=%ld "
                          "threshold=%u spike_rej=%lu sat_rej=%lu noisy_pre_rej=%lu "
                          "sat_pct=%u%% (gate=%d, sat_max=%d%%, pre_max=%u)",
                     window_max, (unsigned long)window_steps,
                     (long)window_pcm_peak, (long)rms,
                     wake_word_get_threshold(),
                     (unsigned long)window_rejected_spikes,
                     (unsigned long)window_saturation_rejects,
                     (unsigned long)window_noisy_pre_rejects,
                     (unsigned)sat_pct,
                     WAKE_CONSECUTIVE_STEPS, SAT_REJECT_PCT,
                     QUIET_MAX_MEANSQ);
            window_max       = 0;
            window_steps     = 0;
            window_pcm_peak  = 0;
            window_pcm_ms    = 0;
            window_samples   = 0;
            window_rejected_spikes = 0;
            window_saturation_rejects = 0;
            window_noisy_pre_rejects = 0;
            last_log_us      = now;
        }

        if (fire) {
            // Saturation gate: if > SAT_REJECT_PCT of samples in the last ~1 s
            // were clipped, reject this detection as a loud-speech FP. Real
            // wake words at normal room volume clip <1%; shouting / loud
            // conversation typically clips 10-50%. See SAT_REJECT_PCT comment.
            if (sat_pct >= SAT_REJECT_PCT) {
                ESP_LOGW(TAG, ">>> REJECTED detection: score=%d but last 1s is "
                              "%u%% clipped (>=%d%%) — loud-speech FP, not wake",
                         score, (unsigned)sat_pct, SAT_REJECT_PCT);
                window_saturation_rejects++;
                consec_above = 0;
                fire = false;
            }
        }

        // Quiet-before gate: even if the current moment isn't clipped, check
        // that the window 500-1000 ms *before* the fire was quiet. Real
        // wake-word attempts come after a pause; FPs embedded in continuous
        // loud conversation don't. Averages meansq over chunks [sat_ring_idx,
        // sat_ring_idx + QUIET_PRE_SPAN) — the oldest half of the ring, which
        // represents 500-1000 ms pre-fire (the wake word itself occupies the
        // most recent ~500 ms and is excluded).
        uint32_t pre_avg_meansq = 0;
        if (fire) {
            uint64_t pre_sum = 0;
            for (int i = 0; i < QUIET_PRE_SPAN; ++i) {
                int idx = (sat_ring_idx + i) % SAT_WINDOW_CHUNKS;
                pre_sum += meansq_ring[idx];
            }
            pre_avg_meansq = (uint32_t)(pre_sum / QUIET_PRE_SPAN);
            if (pre_avg_meansq > QUIET_MAX_MEANSQ) {
                ESP_LOGW(TAG, ">>> REJECTED detection: score=%d but pre-window "
                              "(1000-1500 ms ago) meansq=%u > %u — no preceding "
                              "pause, likely mid-conversation FP",
                         score, (unsigned)pre_avg_meansq, QUIET_MAX_MEANSQ);
                window_noisy_pre_rejects++;
                consec_above = 0;
                fire = false;
            }
        }

        if (fire) {
            ESP_LOGI(TAG, ">>> WAKE WORD DETECTED <<< score=%d (after %lu consec >= %u, sat=%u%%, pre_meansq=%u)",
                     score, (unsigned long)consec_above,
                     wake_word_get_threshold(), (unsigned)sat_pct,
                     (unsigned)pre_avg_meansq);
            consec_above = 0;
            xEventGroupSetBits(s_trigger_events, s_trigger_bit);
            wake_word_reset();
            atomic_store(&s_paused, true);
            vTaskDelay(pdMS_TO_TICKS(POST_DETECT_MUTE_MS));
            wake_word_reset();
            atomic_store(&s_warmup_left, WARMUP_STEPS_AFTER_RESET);
            atomic_store(&s_paused, false);
        }
    }
}

esp_err_t wake_word_task_start(EventGroupHandle_t events, EventBits_t bit)
{
    if (!events) return ESP_ERR_INVALID_ARG;
    s_trigger_events = events;
    s_trigger_bit    = bit;

    BaseType_t ok = xTaskCreate(wake_word_task_fn, "wake_word", 8192, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void wake_word_task_pause(void)  { atomic_store(&s_paused, true); }
void wake_word_task_resume(void)
{
    // Clear streaming state before unpausing so the first inference step after
    // resume sees a fresh model context, not one flavored by whatever we
    // accumulated right before the pause. Also reset warmup credits so we
    // silently consume the first 200 ms of post-resume scores.
    wake_word_reset();
    atomic_store(&s_warmup_left, WARMUP_STEPS_AFTER_RESET);
    atomic_store(&s_paused, false);
}

uint8_t wake_word_task_last_score(void)
{
    return (uint8_t)atomic_load(&s_last_score);
}
