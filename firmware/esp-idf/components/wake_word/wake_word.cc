// Wake-word inference layer. Loads the embedded streaming tflite model into a
// TFLM MicroInterpreter, dispatches PCM through the selected feature frontend,
// and runs Invoke() each time the model's input window fills.
//
// The streaming model has internal state (LSTM/conv) — we only need to feed it
// new feature frames in stride order; it remembers everything else.

extern "C" {
#include "wake_word.h"
#include "wake_word_frontend.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <string.h>
}

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Embedded model — see EMBED_FILES in CMakeLists.txt.
extern "C" const uint8_t yo_demerzel_v1_tflite_start[] asm("_binary_yo_demerzel_v1_tflite_start");
extern "C" const uint8_t yo_demerzel_v1_tflite_end[]   asm("_binary_yo_demerzel_v1_tflite_end");

static const char *TAG = "wake_word";

namespace {

// Observed arena_used_bytes after AllocateTensors on yo_demerzel_v1 = 33840.
// 64 KB gives a ~30 KB safety margin for model variations.
constexpr size_t TENSOR_ARENA_BYTES = 64 * 1024;
constexpr size_t MAX_FRAMES_PER_FEED = 128;  // 128 * 10 ms = 1.28 s of audio per call

// The trained yo_demerzel_v1 model uses exactly these 13 ops (verified by
// parsing the .tflite flatbuffer). If a future retrain adds new ops,
// AllocateTensors will fail with "Didn't find op for builtin opcode X" —
// add op X here and bump OP_RESOLVER_SIZE.
constexpr int OP_RESOLVER_SIZE = 13;

// Number of resource variables the model may use. Our model (yo_demerzel_v1,
// mixednet streaming with internal state) has one VAR_HANDLE per streaming
// layer — 8 is comfortable headroom; bump if AllocateTensors complains about
// running out.
constexpr int NUM_RESOURCE_VARIABLES = 16;

// Model + interpreter state.
const tflite::Model            *g_model              = nullptr;
tflite::MicroAllocator         *g_allocator          = nullptr;
tflite::MicroResourceVariables *g_resource_variables = nullptr;
tflite::MicroInterpreter       *g_interpreter        = nullptr;
TfLiteTensor                   *g_input              = nullptr;
TfLiteTensor                   *g_output             = nullptr;
uint8_t                        *g_arena              = nullptr;

const wake_word_frontend_t *g_frontend  = nullptr;

// Streaming-model input geometry — the input tensor accepts N stride steps
// before each Invoke(). We discover N from input->dims at init time.
int g_max_stride_steps   = 1;
int g_current_stride_step = 0;

uint8_t g_threshold  = 220;  // ~0.86 sigmoid; raised 2026-04-23 after
                              // false-positive fires in the den. Real-wake
                              // peaks are 217-251, so a few low-scoring
                              // real wakes may miss — iterate up if false
                              // positives persist, down if real wakes miss.
uint8_t g_last_score = 0;

// Scratch for one feed() call's features.
int8_t g_features[MAX_FRAMES_PER_FEED * WAKE_WORD_FEATURE_SIZE];

const wake_word_frontend_t *resolve_frontend(wake_word_frontend_id_t id) {
    switch (id) {
        case WAKE_WORD_FRONTEND_HANDROLL: return &wake_word_frontend_handroll;
        case WAKE_WORD_FRONTEND_TFLM:
        default:                          return &wake_word_frontend_tflm;
    }
}

// Push one feature frame into the input tensor at the current stride slot.
// When the tensor is full, Invoke() and read the new score.
void push_feature_frame(const int8_t *frame_40)
{
    int8_t *input_data = tflite::GetTensorData<int8_t>(g_input);
    memcpy(input_data + g_current_stride_step * WAKE_WORD_FEATURE_SIZE,
           frame_40, WAKE_WORD_FEATURE_SIZE);
    ++g_current_stride_step;

    if (g_current_stride_step < g_max_stride_steps) return;

    g_current_stride_step = 0;
    if (g_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "Invoke failed");
        return;
    }
    g_last_score = g_output->data.uint8[0];
}

}  // namespace

extern "C" {

esp_err_t wake_word_init(wake_word_frontend_id_t frontend)
{
    if (g_interpreter) return ESP_OK;

    g_frontend = resolve_frontend(frontend);
    esp_err_t r = g_frontend->init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "frontend %s init failed: %s", g_frontend->name, esp_err_to_name(r));
        return r;
    }

    g_model = tflite::GetModel(yo_demerzel_v1_tflite_start);
    if (g_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema %lu != supported %d",
                 (unsigned long)g_model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    // Exact op set used by yo_demerzel_v1.tflite (confirmed via flatbuffer
    // inspection). Keep this list in sync with the model's OperatorCodes.
    static tflite::MicroMutableOpResolver<OP_RESOLVER_SIZE> resolver;
    resolver.AddCallOnce();
    resolver.AddVarHandle();
    resolver.AddReshape();
    resolver.AddReadVariable();
    resolver.AddConcatenation();
    resolver.AddStridedSlice();
    resolver.AddAssignVariable();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddSplitV();
    resolver.AddFullyConnected();
    resolver.AddLogistic();
    resolver.AddQuantize();

    // Arena in DRAM (internal heap) — TFLM has had PSRAM alignment issues on
    // ESP32-S3, and 256 KB fits comfortably in internal heap.
    g_arena = (uint8_t *)heap_caps_malloc(TENSOR_ARENA_BYTES,
                                           MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!g_arena) {
        ESP_LOGW(TAG, "DRAM arena %u alloc failed, falling back to PSRAM", (unsigned)TENSOR_ARENA_BYTES);
        g_arena = (uint8_t *)heap_caps_malloc(TENSOR_ARENA_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!g_arena) {
        ESP_LOGE(TAG, "tensor arena alloc failed (%u bytes)", (unsigned)TENSOR_ARENA_BYTES);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "tensor arena: %u bytes @ %p (DRAM=%d)",
             (unsigned)TENSOR_ARENA_BYTES, g_arena,
             esp_ptr_internal(g_arena) ? 1 : 0);

    // Build MicroAllocator + MicroResourceVariables ourselves so the
    // interpreter can resolve VAR_HANDLE / ASSIGN_VARIABLE / READ_VARIABLE
    // ops that the streaming model uses to carry state across Invokes.
    g_allocator = tflite::MicroAllocator::Create(g_arena, TENSOR_ARENA_BYTES);
    if (!g_allocator) {
        ESP_LOGE(TAG, "MicroAllocator::Create failed");
        heap_caps_free(g_arena); g_arena = nullptr;
        return ESP_FAIL;
    }

    g_resource_variables = tflite::MicroResourceVariables::Create(
        g_allocator, NUM_RESOURCE_VARIABLES);
    if (!g_resource_variables) {
        ESP_LOGE(TAG, "MicroResourceVariables::Create failed");
        heap_caps_free(g_arena); g_arena = nullptr;
        g_allocator = nullptr;
        return ESP_FAIL;
    }

    static tflite::MicroInterpreter interp(g_model, resolver, g_allocator,
                                            g_resource_variables);
    g_interpreter = &interp;

    if (g_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed — try larger arena or add missing op to resolver");
        heap_caps_free(g_arena);
        g_arena = nullptr;
        g_interpreter = nullptr;
        g_allocator = nullptr;
        g_resource_variables = nullptr;
        return ESP_FAIL;
    }

    g_input  = g_interpreter->input(0);
    g_output = g_interpreter->output(0);

    // Sanity log of tensor shapes.
    int input_total = 1;
    for (int i = 0; i < g_input->dims->size; ++i) input_total *= g_input->dims->data[i];
    g_max_stride_steps = input_total / WAKE_WORD_FEATURE_SIZE;
    if (g_max_stride_steps < 1) g_max_stride_steps = 1;

    ESP_LOGI(TAG, "model loaded: %u bytes, arena_used=%u, frontend=%s",
             (unsigned)(yo_demerzel_v1_tflite_end - yo_demerzel_v1_tflite_start),
             (unsigned)g_interpreter->arena_used_bytes(),
             g_frontend->name);
    ESP_LOGI(TAG, "input shape: dims=%d  total=%d  → %d stride steps × %d features",
             g_input->dims->size, input_total, g_max_stride_steps, WAKE_WORD_FEATURE_SIZE);
    ESP_LOGI(TAG, "output type=%d  bytes=%u", g_output->type, (unsigned)g_output->bytes);

    g_current_stride_step = 0;
    g_last_score = 0;
    return ESP_OK;
}

void wake_word_deinit(void)
{
    if (g_frontend) g_frontend->deinit();
    if (g_arena) {
        heap_caps_free(g_arena);
        g_arena = nullptr;
    }
    g_interpreter = nullptr;
    g_model = nullptr;
    g_input = nullptr;
    g_output = nullptr;
    g_frontend = nullptr;
}

void wake_word_reset(void)
{
    if (g_frontend) g_frontend->reset();
    // Zero the persistent model state (VAR_HANDLE / ASSIGN_VARIABLE tensors).
    // Without this, streaming state from before a pause (e.g. while TTS was
    // playing) carries into the next Invoke and reliably produces score=255
    // on the first step after resume. Reset is cheap — microseconds.
    if (g_resource_variables) g_resource_variables->ResetAll();
    g_current_stride_step = 0;
    g_last_score = 0;
}

int wake_word_feed(const int16_t *pcm, size_t n_samples, size_t *steps_out)
{
    if (!g_interpreter || !g_frontend || !pcm || n_samples == 0) {
        if (steps_out) *steps_out = 0;
        return g_last_score;
    }

    uint8_t peak = 0;
    size_t steps = 0;

    // hop_samples = 16000 Hz * 10 ms / 1000 = 160. Process in chunks no larger
    // than what fits in the feature scratch buffer.
    constexpr size_t HOP_SAMPLES   = (16000 * WAKE_WORD_FEATURE_HOP_MS) / 1000;
    constexpr size_t PCM_PER_CHUNK = MAX_FRAMES_PER_FEED * HOP_SAMPLES;

    size_t offset = 0;
    while (offset < n_samples) {
        size_t take = n_samples - offset;
        if (take > PCM_PER_CHUNK) take = PCM_PER_CHUNK;

        size_t frames = g_frontend->process(pcm + offset, take,
                                             g_features, MAX_FRAMES_PER_FEED);
        for (size_t i = 0; i < frames; ++i) {
            push_feature_frame(g_features + i * WAKE_WORD_FEATURE_SIZE);
            if (g_current_stride_step == 0) {  // an Invoke just fired
                ++steps;
                if (g_last_score > peak) peak = g_last_score;
            }
        }
        offset += take;
    }

    if (steps_out) *steps_out = steps;
    return peak;
}

void wake_word_set_threshold(uint8_t t) { g_threshold = t; }
uint8_t wake_word_get_threshold(void)   { return g_threshold; }
bool    wake_word_detected(void)        { return g_last_score >= g_threshold; }
uint8_t wake_word_last_score(void)      { return g_last_score; }

size_t wake_word_extract_features(const int16_t *pcm, size_t n_samples,
                                   int8_t *features_out, size_t max_frames)
{
    if (!g_frontend || !pcm || !features_out) return 0;
    return g_frontend->process(pcm, n_samples, features_out, max_frames);
}

}  // extern "C"
