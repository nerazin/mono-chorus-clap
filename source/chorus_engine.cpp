#include "chorus_engine.h"
#include "friend/core/Chorus.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
constexpr double kNativeRate = 44100.0;
constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kScale24 = 8388608.0;
constexpr int kControls = 8;
constexpr int kNativeBlock = 16;
constexpr int kHistory = 2048;    // power of two; larger than the longest FIR
constexpr int kPhases = 128;
constexpr int kQueue = 8192;      // power of two, larger than any output burst

struct Frame {
    double left = 0.0, right = 0.0;
    double controls[kControls]{};
};

// Streaming, band-limited sample-rate conversion. A windowed-sinc table is
// prepared during activation; the audio callback performs no allocations.
// Both the input signal and the corresponding control timeline travel through
// the input converter so automation stays aligned with the source audio.
class Resampler {
public:
    ~Resampler() { std::free(coefficients_); }

    bool prepare(double input_rate, double output_rate) {
        ratio_ = input_rate / output_rate;
        half_ = static_cast<int>(std::ceil(40.0 * std::max(1.0, ratio_)));
        taps_ = 2 * half_;
        if (taps_ + 2 >= kHistory) return false;
        coefficients_ = static_cast<float *>(std::calloc(size_t(kPhases + 1) * size_t(taps_), sizeof(float)));
        if (!coefficients_) return false;
        const double cutoff = 0.95 * std::min(1.0, output_rate / input_rate);
        for (int phase = 0; phase <= kPhases; ++phase) {
            const double fraction = double(phase) / kPhases;
            double sum = 0.0;
            for (int k = 0; k < taps_; ++k) {
                const double d = double(k - half_ + 1) - fraction;
                const double x = d / half_;
                const double window = std::abs(x) <= 1.0
                    ? 0.42 + 0.5 * std::cos(kPi * x) + 0.08 * std::cos(2.0 * kPi * x) : 0.0;
                const double u = kPi * cutoff * d;
                const double sinc = std::abs(u) < 1e-14 ? 1.0 : std::sin(u) / u;
                const double v = cutoff * sinc * window;
                coefficients_[size_t(phase) * size_t(taps_) + size_t(k)] = float(v);
                sum += v;
            }
            if (sum != 0.0) {
                for (int k = 0; k < taps_; ++k)
                    coefficients_[size_t(phase) * size_t(taps_) + size_t(k)] /= float(sum);
            }
        }
        clear();
        return true;
    }

    void clear() {
        for (Frame &frame : history_) frame = Frame{};
        last_ = -1;
        output_index_ = 0;
    }

    int half() const { return half_; }

    void push(const Frame &frame) {
        ++last_;
        history_[size_t(last_) & (kHistory - 1)] = frame;
    }

    bool ready() const {
        const double position = double(output_index_) * ratio_;
        return static_cast<int64_t>(std::floor(position)) + half_ <= last_;
    }

    Frame pop() {
        const double position = double(output_index_++) * ratio_;
        const int64_t base = static_cast<int64_t>(std::floor(position));
        const double phase = (position - double(base)) * kPhases;
        const int p = std::min(kPhases - 1, static_cast<int>(phase));
        const double blend = phase - p;
        const float *a = coefficients_ + size_t(p) * size_t(taps_);
        const float *b = a + taps_;
        Frame out{};
        for (int k = 0; k < taps_; ++k) {
            const int64_t index = base - half_ + 1 + k;
            if (index < 0) continue;
            const Frame &in = history_[size_t(index) & (kHistory - 1)];
            const double weight = a[k] + (double(b[k]) - a[k]) * blend;
            out.left += in.left * weight;
            out.right += in.right * weight;
        }
        if (base >= 0) {
            const Frame &at_time = history_[size_t(base) & (kHistory - 1)];
            std::memcpy(out.controls, at_time.controls, sizeof out.controls);
        }
        return out;
    }

private:
    float *coefficients_ = nullptr;
    Frame history_[kHistory]{};
    double ratio_ = 1.0;
    int half_ = 0, taps_ = 0;
    int64_t last_ = -1;
    uint64_t output_index_ = 0;
};

uint32_t control_word(double value) {
    if (!std::isfinite(value)) value = 0.0;
    value = std::clamp(value, 0.0, 127.0);
    return uint32_t(std::floor(value * 65536.0));
}

uint32_t audio_word(double value) {
    if (!std::isfinite(value)) value = 0.0;
    value = std::clamp(value, -1.0, 1.0 - 1.0 / kScale24);
    const int32_t signed_value = int32_t(std::llround(value * kScale24));
    return monofx::native::word(signed_value);
}

struct OutputPair { double left, right; };

class Engine {
public:
    explicit Engine(double rate) : rate_(rate), direct_(rate == kNativeRate) {}

    bool prepare() {
        if (!std::isfinite(rate_) || rate_ < 8000.0 || rate_ > 384000.0) return false;
        if (direct_) {
            latency_ = kNativeBlock - 1;
        } else {
            if (!to_native_.prepare(rate_, kNativeRate) || !to_host_.prepare(kNativeRate, rate_)) return false;
            latency_ = uint32_t(to_native_.half() + std::ceil((to_host_.half() + kNativeBlock) * rate_ / kNativeRate) + 2);
        }
        reset();
        return true;
    }

    uint32_t latency() const { return latency_; }

    void reset() {
        initialized_ = false;
        block_position_ = 0;
        native_blocks_ = 0;
        host_frames_ = 0;
        read_ = write_ = 0;
        std::memset(input_block_.data(), 0, sizeof(uint32_t) * input_block_.size());
        to_native_.clear();
        to_host_.clear();
    }

    void process(const double in[2], const double controls[8], double out[2]) {
        Frame frame{};
        frame.left = std::isfinite(in[0]) ? in[0] : 0.0;
        frame.right = std::isfinite(in[1]) ? in[1] : 0.0;
        for (int i = 0; i < kControls; ++i) frame.controls[i] = controls[i];

        if (direct_) {
            native_input(frame);
        } else {
            to_native_.push(frame);
            while (to_native_.ready()) native_input(to_native_.pop());
        }

        out[0] = out[1] = 0.0;
        if (host_frames_ >= latency_ && read_ != write_) {
            const OutputPair &sample = output_[size_t(read_++) & (kQueue - 1)];
            out[0] = sample.left;
            out[1] = sample.right;
        }
        ++host_frames_;
    }

private:
    void initialize(const double controls[8]) {
        for (int i = 0; i < kControls; ++i) current_[i] = control_word(controls[i]);
        chorus_.reset(current_);
        // Match 0.3.0's ready-to-play behaviour: run the firmware's 128-block
        // wet-output gate on silence before processing the first host sample.
        monofx::core::StereoBlock silence{};
        for (unsigned i = 0; i < 128; ++i) chorus_.process(silence);
        native_blocks_ = 128;
        initialized_ = true;
    }

    void native_input(const Frame &in) {
        if (!initialized_) initialize(in.controls);
        if (block_position_ == 0) {
            // Model the Monomachine host's control slew. The 52-word controller
            // updates once per 24 DSP blocks, on block 3 of each cycle.
            if (native_blocks_ % 24 == 3) {
                for (int i = 0; i < kControls; ++i) {
                    const uint32_t target = control_word(in.controls[i]);
                    current_[i] = (3u * current_[i] + target) >> 2;
                }
            }
            chorus_.setControls(current_);
        }
        input_block_[size_t(block_position_) * 2] = audio_word(in.left);
        input_block_[size_t(block_position_) * 2 + 1] = audio_word(in.right);
        if (++block_position_ != kNativeBlock) return;
        block_position_ = 0;
        ++native_blocks_;
        const auto &processed = chorus_.process(input_block_);
        for (int i = 0; i < kNativeBlock; ++i) {
            Frame sample{};
            sample.left = monofx::native::signed24(processed[size_t(i) * 2]) / kScale24;
            sample.right = monofx::native::signed24(processed[size_t(i) * 2 + 1]) / kScale24;
            if (direct_) {
                enqueue(sample.left, sample.right);
            } else {
                to_host_.push(sample);
                while (to_host_.ready()) {
                    const Frame converted = to_host_.pop();
                    enqueue(converted.left, converted.right);
                }
            }
        }
    }

    void enqueue(double left, double right) {
        // The latency and capacity bounds guarantee space for the largest
        // possible block burst at every supported rate.
        if (write_ - read_ >= kQueue) return;
        output_[size_t(write_++) & (kQueue - 1)] = {left, right};
    }

    double rate_;
    bool direct_ = false, initialized_ = false;
    uint32_t latency_ = 0;
    int block_position_ = 0;
    uint64_t native_blocks_ = 0, host_frames_ = 0, read_ = 0, write_ = 0;
    monofx::core::Chorus chorus_{};
    monofx::core::ChorusControls current_{};
    monofx::core::StereoBlock input_block_{};
    Resampler to_native_{}, to_host_{};
    OutputPair output_[kQueue]{};
};
}

extern "C" void *chorus_engine_create(double rate) {
    void *memory = std::malloc(sizeof(Engine));
    if (!memory) return nullptr;
    Engine *engine = new (memory) Engine(rate);
    if (!engine->prepare()) {
        engine->~Engine();
        std::free(memory);
        return nullptr;
    }
    return engine;
}

extern "C" void chorus_engine_destroy(void *pointer) {
    if (!pointer) return;
    Engine *engine = static_cast<Engine *>(pointer);
    engine->~Engine();
    std::free(pointer);
}

extern "C" void chorus_engine_reset(void *pointer) {
    if (pointer) static_cast<Engine *>(pointer)->reset();
}

extern "C" uint32_t chorus_engine_latency(const void *pointer) {
    return pointer ? static_cast<const Engine *>(pointer)->latency() : 0;
}

extern "C" void chorus_engine_process(void *pointer, const double input[2], const double controls[8], double output[2]) {
    static_cast<Engine *>(pointer)->process(input, controls, output);
}
