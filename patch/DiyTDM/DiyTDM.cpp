#include <string>
//#include <cassert>

#include "daisy_patch.h"
#include "daisysp.h"

using namespace std;
using namespace daisy;
using namespace daisysp;

#define BIT_SET(a,b) ((a) |= (1ULL<<(b)))
#define BIT_FLIP(a,b) ((a) ^= (1ULL<<(b)))
#define BIT_CHECK(a,b) (!!((a) & (1ULL<<(b))))        // '!!' to make sure this returns 0 or 1
#define BIT_CLEAR(a,b) ((a) &= ~(1ULL<<(b)))
#define BITMASK_FLIP(x, mask) ((x) ^= (mask))

const int NUM_BITS = 12;
const int MAX_VALUE = pow(2, NUM_BITS) - 1;

DaisyPatch hw;

// min and max of the roll range ( ctrl 1 and 2)
Parameter roll_min_param;
Parameter roll_max_param;
int roll_min_v = 0;
int roll_max_v = NUM_BITS-1;

// flip on roll probability ( ctrl 3 )
Parameter flip_on_roll_param;
float flip_on_roll_prob = 0;

// which bit is the cursor controlled by the encoder pointing at?
int cursor = 0;

// flip mask for audio
int flip_mask = 0;

// mask for the roll range
int roll_range_mask = 0;

// latch to set initial values on first call
bool first_update_call = true;


const inline float random_01() {
    // TODO use daisy::Random::GetFloat(); // which defaults to (0, 1)
    return static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
}

inline float clip(const float v) {
    if (v < -1.0) {
        return -1.0;
    } else if (v > 1.0) {
        return 1.0;
    } else {
        return v;
    }
}

inline int to_int(float in_v) {
    // map from (-1, 1) to (0, bitrate) as int
    in_v = clip(in_v);
    return (int)((in_v + 1) / 2 * MAX_VALUE);
}

inline float to_float(const int in_v) {
    // cast to float
    float val_f = (float)in_v;
    // and rescale back to (-1, 1)
    val_f = (val_f / MAX_VALUE * 2) - 1;
    //assert(val_f >= -1.0);
    //assert(val_f <= 1.0);
    return val_f;
}

inline float bitflip(float in_v) {
    // cast to int
    int int_v = to_int(in_v);
    // flip bits
    BITMASK_FLIP(int_v, flip_mask);
    // return as float
    return to_float(int_v);
}

inline int roll(int v, int min_r, int max_r) {
    //assert(min_r < max_r);

    // record original values of mask that are outside the roll range
    const int outside_range = v & ~roll_range_mask;

    // rotate values in roll range, roll left most value over to right most
    const bool left_most_bit_set = BIT_CHECK(v, min_r);
    v >>= 1;
    if (left_most_bit_set) {
        BIT_SET(v, max_r);
    } else {
        BIT_CLEAR(v, max_r);
    }

    // potentially flip rolled bit
    // avoid slow random_01() call for boundary cases of never and always
    if (flip_on_roll_prob != 0) {
        if (flip_on_roll_prob == 1 || (random_01() < flip_on_roll_prob)) {
            BIT_FLIP(v, max_r);
        }
    }

    // combined rolled values with the original unrolled values
    return (v & roll_range_mask) | outside_range;
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    for (size_t i = 0; i < size; i++) {
        out[0][i] = bitflip(in[0][i]);
        out[1][i] = in[1][i];
        out[2][i] = in[2][i];
        out[3][i] = in[3][i];
    }
}

void UpdateControls() {
    hw.ProcessAllControls();

    float ctrl_v;

    // moving encoder moves cursor for mask bit flipping
    cursor += hw.encoder.Increment();
    if (cursor < 0) {
        cursor = 0;
    } else if (cursor > NUM_BITS-1) {
        cursor = NUM_BITS-1;
    }

    // clicking encoder toggles mask bit state
    if (hw.encoder.RisingEdge()) {
        BIT_FLIP(flip_mask, cursor);
    }

    // set min and max of the roll range from ctrls 1 and 2
    // ensure that min < max
    const int initial_roll_min_v = roll_min_v;
    const int initial_roll_max_v = roll_max_v;
    ctrl_v = roll_min_param.Process();
    roll_min_v = (int)(ctrl_v * NUM_BITS);
    if (roll_min_v >= roll_max_v) {
        roll_min_v = roll_max_v-1;
    }
    ctrl_v = roll_max_param.Process();
    roll_max_v = (int)(ctrl_v * NUM_BITS);
    if (roll_max_v <= roll_min_v) {
        roll_max_v = roll_min_v+1;
    } else if (roll_max_v > NUM_BITS-1) {
        roll_max_v = NUM_BITS-1;
    }
    // if min or max changed (or it's the first call) then update roll range mask
    if (first_update_call || roll_min_v != initial_roll_min_v || roll_max_v != initial_roll_max_v) {
        // create range mask
        int mask = 0;
        for (int i=roll_min_v; i<=roll_max_v; i++) {
            BIT_SET(mask, i);
        }
        roll_range_mask = mask;
        first_update_call = false;
    }

    // update flip on roll probability (ctrl3)
    // snap at ends to clean {0, 1}
    ctrl_v = flip_on_roll_param.Process();
    if (ctrl_v < 0.02) {
        flip_on_roll_prob = 0.0;
    } else if (ctrl_v > 0.98) {
        flip_on_roll_prob = 1.0;
    } else {
        flip_on_roll_prob = ctrl_v;
    }

    // gate1 trigger rolls flip mask left
    if (hw.gate_input[0].Trig()) {
        flip_mask = roll(flip_mask, roll_min_v, roll_max_v);
    }

}

void DisplayLines(const vector<string> &strs) {
    int line_num = 0;
    for (string str : strs) {
        char* cstr = &str[0];
        hw.display.SetCursor(0, line_num*10);
        hw.display.WriteString(cstr, Font_7x10, true);
        line_num++;
    }
}

void UpdateDisplay() {
    hw.display.Fill(false);
    vector<string> strs;

    strs.push_back("turindrezmachine");

    // TODO: these first three would be faster just setting chars
    //       in an char[], there's no float parsing or anything.

    // show the encoder cursor position with a 'v'
    FixedCapStr<18> str("");
    for (int i=0; i<NUM_BITS; i++) {
        if (i==cursor) {
            str.Append("v");
        } else {
            str.Append(" ");
        }
    }
    strs.push_back(string(str));

    // show bits of mask, '*' for set, '.' for unset
    str.Clear();
    for (int i=0; i<NUM_BITS; i++) {
        if (BIT_CHECK(flip_mask, i)) {
            str.Append("*");
        } else {
            str.Append(".");
        }
    }
    strs.push_back(string(str));

    // show the roll range using '<' for min and '>' for max
    str.Clear();
    for (int i=0; i<NUM_BITS; i++) {
        if (i==roll_min_v) {
            str.Append(">");
        } else if (i==roll_max_v) {
            str.Append("<");
        } else {
            str.Append(" ");
        }
    }
    strs.push_back(string(str));

    // display the flip probability
    str.Clear();
    str.Append("flip prob ");
    str.AppendFloat(flip_on_roll_prob, 2);
    strs.push_back(string(str));

    DisplayLines(strs);
    hw.display.Update();
}

int main(void) {
    hw.Init();
    hw.SetAudioBlockSize(4); // number of samples handled per callback
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    roll_min_param.Init(hw.controls[0], 0.0f, 1.0f, Parameter::LINEAR);
    roll_max_param.Init(hw.controls[1], 0.0f, 1.0f, Parameter::LINEAR);
    flip_on_roll_param.Init(hw.controls[2], 0.0f, 1.0f, Parameter::LINEAR);

    while(true) {
        for (size_t i = 0; i < 1000; i++) {
            UpdateControls();
        }
        UpdateDisplay();
    }
}
