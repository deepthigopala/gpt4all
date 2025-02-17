#define LLAMAMODEL_H_I_KNOW_WHAT_I_AM_DOING_WHEN_INCLUDING_THIS_FILE
#include "llamamodel_impl.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <iostream>
#if defined(_WIN32) && defined(_MSC_VER)
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <io.h>
    #include <stdio.h>
#else
    #include <unistd.h>
#endif
#include <random>
#include <thread>
#include <unordered_set>

#include <llama.h>
#include <ggml.h>

#ifdef GGML_USE_KOMPUTE
#include "ggml-vulkan.h"
#endif

namespace {
const char *modelType_ = "LLaMA";
}

static bool llama_verbose() {
    const char* var = getenv("GPT4ALL_VERBOSE_LLAMACPP");
    return var && *var;
}

static void llama_log_callback(enum ggml_log_level level, const char *text, void *userdata) {
    (void)userdata;
    if (llama_verbose() || level <= GGML_LOG_LEVEL_ERROR) {
        fputs(text, stderr);
    }
}

struct gpt_params {
    int32_t seed          = -1;   // RNG seed
    int32_t n_keep        = 0;    // number of tokens to keep from initial prompt

    // sampling parameters
    float   tfs_z         = 1.0f; // 1.0 = disabled
    float   typical_p     = 1.0f; // 1.0 = disabled

    std::string prompt = "";

    bool memory_f16        = true;  // use f16 instead of f32 for memory kv

    bool use_mmap          = true;  // use mmap for faster loads
    bool use_mlock         = false; // use mlock to keep model in memory
};

static int llama_sample_top_p_top_k(
        llama_context *ctx,
        const llama_token *last_n_tokens_data,
        int last_n_tokens_size,
        int top_k,
        float top_p,
        float temp,
        float repeat_penalty,
        int32_t pos) {
    auto logits = llama_get_logits_ith(ctx, pos);
    auto n_vocab = llama_n_vocab(llama_get_model(ctx));
    // Populate initial list of all candidates
    std::vector<llama_token_data> candidates;
    candidates.reserve(n_vocab);
    for (int token_id = 0; token_id < n_vocab; token_id++) {
        candidates.emplace_back(llama_token_data{token_id, logits[token_id], 0.0f});
    }
    llama_token_data_array candidates_p = {candidates.data(), candidates.size(), false};
    // Sample repeat penalty
    llama_sample_repetition_penalties(nullptr, &candidates_p, last_n_tokens_data, last_n_tokens_size, repeat_penalty, 0.0f, 0.0f);
    // Temperature sampling
    llama_sample_top_k(ctx, &candidates_p, top_k, 1);
    llama_sample_tail_free(ctx, &candidates_p, 1.0f, 1);
    llama_sample_typical(ctx, &candidates_p, 1.0f, 1);
    llama_sample_top_p(ctx, &candidates_p, top_p, 1);
    llama_sample_temp(ctx, &candidates_p, temp);
    return llama_sample_token(ctx, &candidates_p);
}

struct LLamaPrivate {
    const std::string modelPath;
    bool modelLoaded;
    llama_model *model = nullptr;
    llama_context *ctx = nullptr;
    llama_model_params model_params;
    llama_context_params ctx_params;
    int64_t n_threads = 0;
    std::vector<LLModel::Token> end_tokens;
};

LLamaModel::LLamaModel()
    : d_ptr(new LLamaPrivate) {
    d_ptr->modelLoaded = false;
}

// default hparams (LLaMA 7B)
struct llama_file_hparams {
    uint32_t n_vocab = 32000;
    uint32_t n_embd  = 4096;
    uint32_t n_mult  = 256;
    uint32_t n_head  = 32;
    uint32_t n_layer = 32;
    uint32_t n_rot   = 64;
    enum llama_ftype ftype = LLAMA_FTYPE_MOSTLY_F16;
};

size_t LLamaModel::requiredMem(const std::string &modelPath) {
    auto fin = std::ifstream(modelPath, std::ios::binary);
    fin.seekg(0, std::ios_base::end);
    size_t filesize = fin.tellg();
    fin.seekg(0, std::ios_base::beg);
    uint32_t magic = 0;
    fin.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != 0x67676a74) return 0;
    uint32_t version = 0;
    fin.read(reinterpret_cast<char*>(&version), sizeof(version));
    llama_file_hparams hparams;
    fin.read(reinterpret_cast<char*>(&hparams.n_vocab), sizeof(hparams.n_vocab));
    fin.read(reinterpret_cast<char*>(&hparams.n_embd), sizeof(hparams.n_embd));
    fin.read(reinterpret_cast<char*>(&hparams.n_head), sizeof(hparams.n_head));
    fin.read(reinterpret_cast<char*>(&hparams.n_layer), sizeof(hparams.n_layer));
    fin.read(reinterpret_cast<char*>(&hparams.n_rot), sizeof(hparams.n_rot));
    fin.read(reinterpret_cast<char*>(&hparams.ftype), sizeof(hparams.ftype));
    const size_t n_ctx = 2048;
    const size_t kvcache_element_size = 2; // fp16
    const size_t est_kvcache_size = hparams.n_embd * hparams.n_layer * 2u * n_ctx * kvcache_element_size;
    return filesize + est_kvcache_size;
}

bool LLamaModel::loadModel(const std::string &modelPath)
{
    gpt_params params;

    // load the model
    d_ptr->model_params = llama_model_default_params();

    d_ptr->model_params.use_mmap   = params.use_mmap;
#if defined (__APPLE__)
    d_ptr->model_params.use_mlock  = true;
#else
    d_ptr->model_params.use_mlock  = params.use_mlock;
#endif

    d_ptr->ctx_params = llama_context_default_params();

    d_ptr->ctx_params.n_ctx  = 2048;
    d_ptr->ctx_params.seed   = params.seed;
    d_ptr->ctx_params.f16_kv = params.memory_f16;

    // The new batch API provides space for n_vocab*n_tokens logits. Tell llama.cpp early
    // that we want this many logits so the state serializes consistently.
    d_ptr->ctx_params.logits_all = true;

    d_ptr->n_threads = std::min(4, (int32_t) std::thread::hardware_concurrency());
    d_ptr->ctx_params.n_threads       = d_ptr->n_threads;
    d_ptr->ctx_params.n_threads_batch = d_ptr->n_threads;

#ifdef GGML_USE_METAL
    if (llama_verbose()) {
        std::cerr << "llama.cpp: using Metal" << std::endl;
    }
    // metal always runs the whole model if n_gpu_layers is not 0, at least
    // currently
    d_ptr->model_params.n_gpu_layers = 1;
#endif
#ifdef GGML_USE_KOMPUTE
    if (ggml_vk_has_device()) {
        // vulkan always runs the whole model if n_gpu_layers is not 0, at least
        // currently
        d_ptr->model_params.n_gpu_layers = 1;
    }
#endif

    d_ptr->model = llama_load_model_from_file_gpt4all(modelPath.c_str(), &d_ptr->model_params);
    if (!d_ptr->model) {
#ifdef GGML_USE_KOMPUTE
        // Explicitly free the device so next load it doesn't use it
        ggml_vk_free_device();
#endif
        std::cerr << "LLAMA ERROR: failed to load model from " <<  modelPath << std::endl;
        return false;
    }

    d_ptr->ctx = llama_new_context_with_model(d_ptr->model, d_ptr->ctx_params);
    if (!d_ptr->ctx) {
#ifdef GGML_USE_KOMPUTE
        // Explicitly free the device so next load it doesn't use it
        ggml_vk_free_device();
#endif
        std::cerr << "LLAMA ERROR: failed to init context for model " <<  modelPath << std::endl;
        return false;
    }

    d_ptr->end_tokens = {llama_token_eos(d_ptr->model)};

#ifdef GGML_USE_KOMPUTE
    if (ggml_vk_has_device()) {
        std::cerr << "llama.cpp: using Vulkan on " << ggml_vk_current_device().name << std::endl;
    }
#endif

    d_ptr->modelLoaded = true;
    fflush(stderr);
    return true;
}

void LLamaModel::setThreadCount(int32_t n_threads) {
    d_ptr->n_threads = n_threads;
    llama_set_n_threads(d_ptr->ctx, n_threads, n_threads);
}

int32_t LLamaModel::threadCount() const {
    return d_ptr->n_threads;
}

LLamaModel::~LLamaModel()
{
    if (d_ptr->ctx) {
        llama_free(d_ptr->ctx);
    }
    llama_free_model(d_ptr->model);
}

bool LLamaModel::isModelLoaded() const
{
    return d_ptr->modelLoaded;
}

size_t LLamaModel::stateSize() const
{
    return llama_get_state_size(d_ptr->ctx);
}

size_t LLamaModel::saveState(uint8_t *dest) const
{
    return llama_copy_state_data(d_ptr->ctx, dest);
}

size_t LLamaModel::restoreState(const uint8_t *src)
{
    // const_cast is required, see: https://github.com/ggerganov/llama.cpp/pull/1540
    return llama_set_state_data(d_ptr->ctx, const_cast<uint8_t*>(src));
}

std::vector<LLModel::Token> LLamaModel::tokenize(PromptContext &ctx, const std::string &str) const
{
    const bool useBOS = ctx.n_past == 0 && (ctx.tokens.empty() || ctx.tokens.front() != llama_token_bos(d_ptr->model));
    std::vector<LLModel::Token> fres(str.size()+4);
    // TODO(cebtenzzre): we may want to use special=true here to process special tokens
    auto fres_len = llama_tokenize(d_ptr->model, str.c_str(), str.length(), fres.data(), fres.size(), useBOS, false);
    fres.resize(fres_len);
    return fres;
}

std::string LLamaModel::tokenToString(Token id) const
{
    return llama_token_to_piece(d_ptr->ctx, id);
}

LLModel::Token LLamaModel::sampleToken(PromptContext &promptCtx) const
{
    const size_t n_prev_toks = std::min((size_t) promptCtx.repeat_last_n, promptCtx.tokens.size());
    return llama_sample_top_p_top_k(d_ptr->ctx,
        promptCtx.tokens.data() + promptCtx.tokens.size() - n_prev_toks,
        n_prev_toks, promptCtx.top_k, promptCtx.top_p, promptCtx.temp,
        promptCtx.repeat_penalty, promptCtx.n_last_batch_tokens - 1);
}

bool LLamaModel::evalTokens(PromptContext &ctx, const std::vector<int32_t> &tokens) const
{
    llama_batch batch = llama_batch_init(tokens.size(), 0, 1);

    batch.n_tokens = tokens.size();
    ctx.n_last_batch_tokens = tokens.size();

    for (int32_t i = 0; i < batch.n_tokens; i++) {
        batch.token   [i] = tokens[i];
        batch.pos     [i] = ctx.n_past + i;
        batch.n_seq_id[i] = 1;
        batch.seq_id  [i][0] = 0;
        batch.logits  [i] = false;
    }

    // llama_decode will output logits only for the last token of the prompt
    batch.logits[batch.n_tokens - 1] = true;

    int res = llama_decode(d_ptr->ctx, batch);
    llama_batch_free(batch);
    return res == 0;
}

int32_t LLamaModel::contextLength() const
{
    return llama_n_ctx(d_ptr->ctx);
}

const std::vector<LLModel::Token> &LLamaModel::endTokens() const
{
    return d_ptr->end_tokens;
}

#if defined(GGML_USE_KOMPUTE)
#include "ggml-vulkan.h"
#endif

std::vector<LLModel::GPUDevice> LLamaModel::availableGPUDevices(size_t memoryRequired)
{
#if defined(GGML_USE_KOMPUTE)
    std::vector<ggml_vk_device> vkDevices = ggml_vk_available_devices(memoryRequired);

    std::vector<LLModel::GPUDevice> devices;
    for(const auto& vkDevice : vkDevices) {
        LLModel::GPUDevice device;
        device.index = vkDevice.index;
        device.type = vkDevice.type;
        device.heapSize = vkDevice.heapSize;
        device.name = vkDevice.name;
        device.vendor = vkDevice.vendor;

        devices.push_back(device);
    }

    return devices;
#else
    return std::vector<LLModel::GPUDevice>();
#endif
}

bool LLamaModel::initializeGPUDevice(size_t memoryRequired, const std::string& device)
{
#if defined(GGML_USE_KOMPUTE)
    return ggml_vk_init_device(memoryRequired, device);
#else
    return false;
#endif
}

bool LLamaModel::initializeGPUDevice(const LLModel::GPUDevice &device, std::string *unavail_reason)
{
    bool result = false;
#if defined(GGML_USE_KOMPUTE)
    ggml_vk_device vkDevice;
    vkDevice.index = device.index;
    vkDevice.type = device.type;
    vkDevice.heapSize = device.heapSize;
    vkDevice.name = device.name;
    vkDevice.vendor = device.vendor;
    result = ggml_vk_init_device(vkDevice);
    if (!result && unavail_reason) {
        *unavail_reason = "failed to init GPU";
    }
#else
    if (unavail_reason) {
        *unavail_reason = "built without Kompute";
    }
#endif
    return result;
}

bool LLamaModel::initializeGPUDevice(int device)
{
#if defined(GGML_USE_KOMPUTE)
    return ggml_vk_init_device(device);
#else
    return false;
#endif
}

bool LLamaModel::hasGPUDevice()
{
#if defined(GGML_USE_KOMPUTE)
    return ggml_vk_has_device();
#else
    return false;
#endif
}

bool LLamaModel::usingGPUDevice()
{
#if defined(GGML_USE_KOMPUTE)
    return ggml_vk_using_vulkan();
#elif defined(GGML_USE_METAL)
    return true;
#endif
    return false;
}

std::string get_arch_name(gguf_context *ctx_gguf) {
    std::string arch_name;
    const int kid = gguf_find_key(ctx_gguf, "general.architecture");
    enum gguf_type ktype = gguf_get_kv_type(ctx_gguf, kid);
    if (ktype != (GGUF_TYPE_STRING)) {
        throw std::runtime_error("ERROR: Can't get general architecture from gguf file.");
    }
    return gguf_get_val_str(ctx_gguf, kid);
}

#if defined(_WIN32)
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT __attribute__ ((visibility ("default")))
#endif

extern "C" {
DLL_EXPORT bool is_g4a_backend_model_implementation() {
    return true;
}

DLL_EXPORT const char *get_model_type() {
    return modelType_;
}

DLL_EXPORT const char *get_build_variant() {
    return GGML_BUILD_VARIANT;
}

DLL_EXPORT bool magic_match(const char * fname) {
    struct ggml_context * ctx_meta = NULL;
    struct gguf_init_params params = {
        /*.no_alloc = */ true,
        /*.ctx      = */ &ctx_meta,
    };
    gguf_context *ctx_gguf = gguf_init_from_file(fname, params);
    if (!ctx_gguf) {
        std::cerr << __func__ << ": gguf_init_from_file failed\n";
        return false;
    }

    bool valid = true;

    int gguf_ver = gguf_get_version(ctx_gguf);
    if (valid && gguf_ver > 3) {
        std::cerr << __func__ << ": unsupported gguf version: " << gguf_ver << "\n";
        valid = false;
    }

    auto arch = get_arch_name(ctx_gguf);
    if (valid && !(arch == "llama" || arch == "starcoder" || arch == "falcon" || arch == "mpt")) {
        if (!(arch == "gptj" || arch == "bert")) { // we support these via other modules
            std::cerr << __func__ << ": unsupported model architecture: " << arch << "\n";
        }
        valid = false;
    }

    gguf_free(ctx_gguf);
    return valid;
}

DLL_EXPORT LLModel *construct() {
    llama_log_set(llama_log_callback, nullptr);
    return new LLamaModel;
}
}
