/* Sympathetic String Reverb LADSPA plugin
 *
 * This plugins tries to emulate a maximum of 11 sympathetic strings using
 * tuned comb filters with a high feedback amount. Result is a sort of metallic
 * sounding reverb. Each of the 11 "strings" can be tuned to an arbitrary
 * frequency to which it will respond the most. Combine with a band-pass filter
 * to get rid of any unwanted frequencies that might lead to ringing effects.
 *
 * Author: Marcus Weseloh <marcus@weseloh.cc>
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <ladspa.h>

#define COMB_COUNT (11)

#define FEEDBACK_OFFSET (0.96f)
#define FEEDBACK_RANGE (0.039f)
#define DAMPING_RANGE (0.5f)

#define PORT_FEEDBACK (COMB_COUNT + 0)
#define PORT_DAMPING (COMB_COUNT + 1)

#define PORT_GAIN_INPUT (COMB_COUNT + 2)
#define PORT_WET_LEFT (COMB_COUNT + 3)
#define PORT_WET_RIGHT (COMB_COUNT + 4)

#define PORT_INPUT  (COMB_COUNT + 5)
#define PORT_OUTPUT1 (COMB_COUNT + 6)
#define PORT_OUTPUT2 (COMB_COUNT + 7)

const LADSPA_Descriptor symp_descriptor;

struct comb {
  float store;
  float *buffer;
  int size;
  int idx;
};

struct symp
{
    LADSPA_Data run_adding_gain;

    LADSPA_Data *ctrl_tunings[COMB_COUNT];
    LADSPA_Data *ctrl_feedback;
    LADSPA_Data *ctrl_damping;
    LADSPA_Data *ctrl_gain_input;
    LADSPA_Data *ctrl_wet_left;
    LADSPA_Data *ctrl_wet_right;
    LADSPA_Data *ctrl_input_gain;
    LADSPA_Data *audio_input;
    LADSPA_Data *audio_output1;
    LADSPA_Data *audio_output2;

    struct comb *combs[COMB_COUNT];
    int num_combs;

    float damping;
    float damp1;
    float damp2;

    float feedback;
    float scaled_feedback;

    unsigned long sample_rate;
};

int symp_setup_combs(struct symp *symp)
{
    int i;
    int size;
    struct comb *comb;

    for (i = 0; i < COMB_COUNT; i++) {
        if (*symp->ctrl_tunings[i] <= 0) continue;

        size = symp->sample_rate / *symp->ctrl_tunings[i];
        comb = malloc(sizeof(struct comb));
        if (comb == NULL) return -1;
        symp->combs[symp->num_combs++] = comb;
        memset(comb, 0, sizeof(struct comb));

        comb->buffer = malloc(size * sizeof(float));
        if (comb->buffer == NULL) return -1;
        memset(comb->buffer, 0, size * sizeof(float));
        comb->size = size;
    }

    return 1;
}

void symp_cleanup_combs(struct symp *symp)
{
    int i;

    for (i = 0; i < symp->num_combs; i++) {
        free(symp->combs[i]);
        symp->combs[i] = NULL;
    }
    symp->num_combs = 0;
}

LADSPA_Handle symp_instantiate(const LADSPA_Descriptor *desc, unsigned long sample_rate)
{
    struct symp *symp;

    symp = malloc(sizeof(struct symp));
    if (symp == NULL) return NULL;
    memset(symp, 0, sizeof(struct symp));

    symp->sample_rate = sample_rate;
    symp->damp2 = 0;
    symp->damp2 = 1;

    return symp;
}

void symp_cleanup(LADSPA_Handle handle)
{
    free(handle);
}

void symp_activate(LADSPA_Handle handle)
{
    struct symp *symp = (struct symp *)handle;

    if (!symp_setup_combs(symp)) {
        printf("Out of memory!\n");
    }
}

void symp_deactivate(LADSPA_Handle handle)
{
    struct symp *symp = (struct symp *)handle;
    symp_cleanup_combs(symp);
}

void symp_connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *buf)
{
    struct symp *symp = (struct symp *)handle;

    /* string tunings */
    if (port < COMB_COUNT) {
        symp->ctrl_tunings[port] = buf;
    }
    else {
        switch (port) {
            case PORT_FEEDBACK:
                symp->ctrl_feedback = buf;
                break;
            case PORT_DAMPING:
                symp->ctrl_damping = buf;
                break;
            case PORT_GAIN_INPUT:
                symp->ctrl_gain_input = buf;
                break;
            case PORT_WET_LEFT:
                symp->ctrl_wet_left = buf;
                break;
            case PORT_WET_RIGHT:
                symp->ctrl_wet_right = buf;
                break;
            case PORT_INPUT:
                symp->audio_input = buf;
                break;
            case PORT_OUTPUT1:
                symp->audio_output1 = buf;
                break;
            case PORT_OUTPUT2:
                symp->audio_output2 = buf;
                break;
        }
    }
}

void inline symp_run_effect(LADSPA_Handle handle, unsigned long sample_count, int add)
{
    struct symp *symp = (struct symp *)handle;
    LADSPA_Data *audio_input = symp->audio_input;
    LADSPA_Data *out1 = symp->audio_output1;
    LADSPA_Data *out2 = symp->audio_output2;
    LADSPA_Data adding_gain = symp->run_adding_gain;
    LADSPA_Data input_gain = *symp->ctrl_gain_input;
    LADSPA_Data wet_left = *symp->ctrl_wet_left;
    LADSPA_Data wet_right = *symp->ctrl_wet_right;
    int i, c;
    struct comb *comb;
    float in, out, tmp, feedback;

    if (wet_left < 0) wet_left = 0;
    else if (wet_left > 1.0) wet_left = 1.0;

    if (wet_right < 0) wet_right = 0;
    else if (wet_right > 1.0) wet_right = 1.0;

    if (*symp->ctrl_damping != symp->damping) {
        symp->damping = *symp->ctrl_damping;
        symp->damp1 = symp->damping * DAMPING_RANGE;
        symp->damp2 = 1 - symp->damp1;
    }

    if (*symp->ctrl_feedback != symp->feedback) {
        symp->feedback = *symp->ctrl_feedback;
        symp->scaled_feedback = FEEDBACK_OFFSET + (symp->feedback * FEEDBACK_RANGE);
    }

    feedback = symp->scaled_feedback;

    for (i = 0; i < sample_count; i++) {
        out = 0.0f;
        in = *audio_input * input_gain;

        for (c = 0; c < symp->num_combs; c++) {
            comb = symp->combs[c];

            tmp = comb->buffer[comb->idx];
            comb->store = (tmp * symp->damp2) + (comb->store * symp->damp1);
            comb->buffer[comb->idx] = in + (comb->store * feedback);
            if (++comb->idx >= comb->size) {
                comb->idx = 0;
            }
            out += tmp;
        }

        if (add) {
            if (wet_left > 0)
                *(out1++) += out * adding_gain * wet_left;
            if (wet_right > 0)
                *(out2++) += out * adding_gain * wet_right;
        } else {
            *(out1++) = out * wet_left;
            *(out2++) = out * wet_right;
        }

        audio_input++;
    }
}

void symp_set_run_adding_gain(LADSPA_Handle handle, LADSPA_Data gain)
{
    struct symp *symp = (struct symp *)handle;
    symp->run_adding_gain = gain;
}

void symp_run(LADSPA_Handle handle, unsigned long sample_count)
{
    symp_run_effect(handle, sample_count, 0);
}

void symp_run_adding(LADSPA_Handle handle, unsigned long sample_count)
{
    symp_run_effect(handle, sample_count, 1);
}

const LADSPA_Descriptor *ladspa_descriptor(unsigned long idx)
{
    switch (idx) {
        case 0:
            return &symp_descriptor;
        default:
            return NULL;
    }
}

const LADSPA_Descriptor symp_descriptor = {
    .UniqueID = 4242,
    .Label = "sympathetic",
    .Name = "Sympathetic String Reverb",
    .Maker = "Marcus Weseloh",
    .Copyright = "GPL",

    .Properties = LADSPA_PROPERTY_HARD_RT_CAPABLE,

    .PortCount = COMB_COUNT + 5 + 3,

    .PortDescriptors = (LADSPA_PortDescriptor[]) {
        /* tuning controls (ports 0-11) */
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,

        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
        LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,

        LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
        LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
        LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO
    },

    .PortNames = (const char *[]) {
        "String1 Tuning",
        "String2 Tuning",
        "String3 Tuning",
        "String4 Tuning",
        "String5 Tuning",
        "String6 Tuning",
        "String7 Tuning",
        "String8 Tuning",
        "String9 Tuning",
        "String10 Tuning",
        "String11 Tuning",

        "Feedback",
        "Damping",
        "Gain Input",
        "Wet Left",
        "Wet Right",

        "Input Mono",
        "Output Left",
        "Output Right"
    },

    .PortRangeHints = (LADSPA_PortRangeHint[]) {
        /* String Tunings */
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MINIMUM, .LowerBound = 262},
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MINIMUM, .LowerBound = 294},
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MINIMUM, .LowerBound = 330},
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MINIMUM, .LowerBound = 349},
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MINIMUM, .LowerBound = 392},
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MINIMUM, .LowerBound = 440},
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MINIMUM, .LowerBound = 494},
        {.HintDescriptor = LADSPA_HINT_DEFAULT_0},
        {.HintDescriptor = LADSPA_HINT_DEFAULT_0},
        {.HintDescriptor = LADSPA_HINT_DEFAULT_0},
        {.HintDescriptor = LADSPA_HINT_DEFAULT_0},

        /* Feedback */
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MIDDLE, .LowerBound = 0.0, .UpperBound = 1.0},
        /* Damping */
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MINIMUM, .LowerBound = 0.0, .UpperBound = 1.0},
        /* Gain Input */
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MINIMUM, .LowerBound = 0.015},
        /* Wet Left */
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MAXIMUM, .LowerBound = 0.0, .UpperBound = 1.0},
        /* Wet Right */
        {.HintDescriptor = LADSPA_HINT_DEFAULT_MAXIMUM, .LowerBound = 0.0, .UpperBound = 1.0},

        /* Audio ports */
        {0}, 
        {0}, 
        {0}, 
    },

    .instantiate = symp_instantiate,
    .connect_port = symp_connect_port,
    .run = symp_run,
    .run_adding = symp_run_adding,
    .set_run_adding_gain = symp_set_run_adding_gain,
    .activate = symp_activate,
    .deactivate = symp_deactivate,
    .cleanup = symp_cleanup
};
