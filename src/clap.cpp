/**
 * CLAP for tuneBfree
 *
 * Based on nakst's CLAP tutorial at:
 *
 *  https://nakst.gitlab.io/tutorial/clap-part-2.html
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <string>

#include "clap/clap.h"
#include "readerwriterqueue.h"
#include "libMTSClient.h"
#ifdef CLAP_GUI
#include <elements.hpp>
namespace ce = cycfi::elements;
#endif

#include "tonegen.h"
#include "overdrive.h"
#include "reverb.h"
#include "whirl.h"

#define MIN(A, B) (((A) < (B)) ? (A) : (B))

// Parameters.
#define P_DRAWBAR_MIN (0)
#define P_DRAWBAR_MAX (8)
#define P_VIBRATO (9)
#define P_VIBRATO_TYPE (10)
#define P_DRUM (11)
#define P_HORN (12)
#define P_OVERDRIVE (13)
#define P_CHARACTER (14)
#define P_REVERB (15)
#define P_PERCUSSION (16)
#define P_PERCUSSION_VOLUME (17)
#define P_PERCUSSION_DECAY (18)
#define P_PERCUSSION_HARMONIC (19)
#define P_RATIO_TOP_MIN (20)
#define P_RATIO_TOP_MAX (28)
#define P_RATIO_BOTTOM_MIN (29)
#define P_RATIO_BOTTOM_MAX (37)
#define P_COUNT (38)

// GUI size.
#define GUI_WIDTH (800)
#define GUI_HEIGHT (650)

#define ON_TEXT(i)                                                                                 \
    [mainParameters](std::string_view text) -> bool {                                              \
        double top = 1.0, bottom = 1.0;                                                            \
        try                                                                                        \
        {                                                                                          \
            parse_ratio(std::string(text), &top, &bottom);                                         \
        }                                                                                          \
        catch (std::exception &)                                                                   \
        {                                                                                          \
            return true;                                                                           \
        }                                                                                          \
        uint32_t top_index = P_RATIO_TOP_MIN + i;                                                  \
        uint32_t bottom_index = P_RATIO_BOTTOM_MIN + i;                                            \
        mainParameters[top_index] = top;                                                           \
        toAudioQ.try_enqueue({top_index, top});                                                    \
        mainParameters[bottom_index] = bottom;                                                     \
        toAudioQ.try_enqueue({bottom_index, bottom});                                              \
        return true;                                                                               \
    }

struct ParamMsg
{
    uint32_t paramIndex;
    double value;
};

static moodycamel::ReaderWriterQueue<ParamMsg, 4096> toAudioQ(128);
static moodycamel::ReaderWriterQueue<ParamMsg, 4096> fromAudioQ(128);

struct MyPlugin
{
    clap_plugin_t plugin;
    const clap_host_t *host;
    float sampleRate;
    double parameters[P_COUNT], mainParameters[P_COUNT];
    struct b_tonegen *synth;
    struct b_preamp *preamp;
    struct b_reverb *reverb;
    struct b_whirl *whirl;
    int boffset;
    float bufA[BUFFER_SIZE_SAMPLES];
    float bufB[BUFFER_SIZE_SAMPLES];
    float bufC[BUFFER_SIZE_SAMPLES];
    float bufD[2][BUFFER_SIZE_SAMPLES]; // drum, tmp.
    float bufL[2][BUFFER_SIZE_SAMPLES]; // leslie, out
    MTSClient *client;
    double previousFrequency[128];
    double previousRatio[NOF_DRAWBARS];
#ifdef CLAP_GUI
    struct GUI *gui;
    const clap_host_posix_fd_support_t *hostPOSIXFDSupport;
#endif
};

void setToneGenParam(b_tonegen *synth, uint32_t index, float value)
{
    if ((P_DRAWBAR_MIN <= index) && (index <= P_DRAWBAR_MAX))
    {
        setDrawBar(synth, index, rint(value));
    }
    else if (index == P_VIBRATO)
    {
        setVibratoUpper(synth, rint(value));
    }
    else if (index == P_VIBRATO_TYPE)
    {
        setVibratoFromInt(synth, floor(value));
    }
}

double getRatio(double *parameters, int i)
{
    return *(parameters + P_RATIO_TOP_MIN + i) / *(parameters + P_RATIO_BOTTOM_MIN + i);
}

void reinitToneGen(MyPlugin *plugin)
{
#ifdef DEBUG_PRINT
    fprintf(stderr, "reinitToneGen\n");
#endif
    unsigned int newRouting = plugin->synth->newRouting;
    freeToneGenerator(plugin->synth);
    plugin->synth = allocTonegen();
    double targetRatio[NOF_DRAWBARS] = {0.0};
    for (int i = 0; i < NOF_DRAWBARS; i++)
    {
        targetRatio[i] = getRatio(plugin->parameters, i);
    }
#ifdef DEBUG_PRINT
    for (int i = 0; i <= 8; i++)
    {
        fprintf(stderr, "\ttargetRatio %d %f\n", i, targetRatio[i]);
    };
#endif
    initToneGenerator(plugin->synth, nullptr, plugin->sampleRate, targetRatio);
    init_vibrato(&(plugin->synth->inst_vibrato), plugin->sampleRate);
    // Restore tonegen parameters after reinitializing tonegen
    for (int i = 0; i < P_COUNT; i++)
    {
        if (((P_DRAWBAR_MIN <= i) && (i <= P_DRAWBAR_MAX)) || (i == P_VIBRATO) ||
            (i == P_VIBRATO_TYPE))
        {
            setToneGenParam(plugin->synth, i, plugin->parameters[i]);
        }
    }
    plugin->synth->newRouting = newRouting;
}

void setParam(MyPlugin *plugin, uint32_t index, float value)
{
    setToneGenParam(plugin->synth, index, value);
    if ((index == P_DRUM) || (index == P_HORN))
    {
        useRevOption(
            plugin->whirl,
            (int)(floor(plugin->parameters[P_DRUM]) + 3 * floor(plugin->parameters[P_HORN])), 2);
    }
    else if (index == P_OVERDRIVE)
    {
        plugin->preamp->isClean = rint(1.0f - value);
    }
    else if (index == P_CHARACTER)
    {
        fsetCharacter(plugin->preamp, value);
    }
    else if (index == P_REVERB)
    {
        setReverbMix(plugin->reverb, value);
    }
    else if (index == P_PERCUSSION)
    {
        setPercussionEnabled(plugin->synth, rint(value));
    }
    else if (index == P_PERCUSSION_VOLUME)
    {
        setPercussionVolume(plugin->synth, 1 - rint(value));
    }
    else if (index == P_PERCUSSION_DECAY)
    {
        setPercussionFast(plugin->synth, rint(value));
    }
    else if (index == P_PERCUSSION_HARMONIC)
    {
        setPercussionFirst(plugin->synth, rint(value));
    }
    else if ((P_RATIO_TOP_MIN <= index) && (index <= P_RATIO_TOP_MAX))
    {
        plugin->synth->targetRatio[index - P_RATIO_TOP_MIN] = value;
    }
    else if ((P_RATIO_BOTTOM_MIN <= index) && (index <= P_RATIO_BOTTOM_MAX))
    {
        plugin->synth->targetRatio[index - P_RATIO_BOTTOM_MIN] = value;
    }
}

static void PluginProcessEvent(MyPlugin *plugin, const clap_event_header_t *event)
{
    if (event->space_id == CLAP_CORE_EVENT_SPACE_ID)
    {
        if (event->type == CLAP_EVENT_NOTE_ON || event->type == CLAP_EVENT_NOTE_OFF ||
            event->type == CLAP_EVENT_NOTE_CHOKE)
        {
            const clap_event_note_t *noteEvent = (const clap_event_note_t *)event;

            if (event->type == CLAP_EVENT_NOTE_ON)
            {
                oscKeyOn(plugin->synth, noteEvent->key, noteEvent->key);
            }
            else
            {
                oscKeyOff(plugin->synth, noteEvent->key, noteEvent->key);
            }
        }
        else if (event->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t *valueEvent = (const clap_event_param_value_t *)event;
            uint32_t index = (uint32_t)valueEvent->param_id;

            plugin->parameters[index] = valueEvent->value;
            struct ParamMsg p = {index, valueEvent->value};
            fromAudioQ.try_enqueue(p);
#ifdef DEBUG_PRINT
            fprintf(stderr, "PluginProcessEvent fromAudioQ.try_enqueue: %d %f\n", p.paramIndex,
                    p.value);
#endif
            setParam(plugin, index, valueEvent->value);
        }
    }
}

static uint32_t synthSound(struct MyPlugin *plugin, uint32_t written, uint32_t nframes,
                           float *outputL, float *outputR)
{
    while (written < nframes)
    {
        int nremain = nframes - written;

        if (plugin->boffset >= BUFFER_SIZE_SAMPLES)
        {
            plugin->boffset = 0;
            oscGenerateFragment(plugin->synth, plugin->bufA, BUFFER_SIZE_SAMPLES);
            preamp(plugin->preamp, plugin->bufA, plugin->bufB, BUFFER_SIZE_SAMPLES);
            plugin->reverb->reverb(plugin->bufB, plugin->bufC, BUFFER_SIZE_SAMPLES);
            whirlProc3(plugin->whirl, plugin->bufC, plugin->bufL[0], plugin->bufL[1],
                       plugin->bufD[0], plugin->bufD[1], BUFFER_SIZE_SAMPLES);
        }

        int nread = MIN(nremain, (BUFFER_SIZE_SAMPLES - plugin->boffset));

        memcpy(&outputL[written], &plugin->bufL[0][plugin->boffset], nread * sizeof(float));
        memcpy(&outputR[written], &plugin->bufL[1][plugin->boffset], nread * sizeof(float));

        written += nread;
        plugin->boffset += nread;
    }
    return written;
}

static void PluginRenderAudio(MyPlugin *plugin, uint32_t start, uint32_t end, float *outputL,
                              float *outputR)
{
    synthSound(plugin, 0, end - start, outputL + start, outputR + start);
}

static void PluginSyncMainToAudio(MyPlugin *plugin, const clap_output_events_t *out)
{
    struct ParamMsg p;
    while (toAudioQ.try_dequeue(p))
    {
#ifdef DEBUG_PRINT
        fprintf(stderr, "PluginSyncMainToAudio toAudioQ.try_dequeue: %d %f\n", p.paramIndex,
                p.value);
#endif
        uint32_t i = p.paramIndex;
        plugin->parameters[i] = p.value;
        setParam(plugin, p.paramIndex, p.value);

        clap_event_param_value_t event = {};
        event.header.size = sizeof(event);
        event.header.time = 0;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_PARAM_VALUE;
        event.header.flags = 0;
        event.param_id = i;
        event.cookie = NULL;
        event.note_id = -1;
        event.port_index = -1;
        event.channel = -1;
        event.key = -1;
        event.value = plugin->parameters[i];
        out->try_push(out, &event.header);
    }
}

static bool PluginSyncAudioToMain(MyPlugin *plugin)
{
    bool anyChanged = false;
    struct ParamMsg p;
    while (fromAudioQ.try_dequeue(p))
    {
#ifdef DEBUG_PRINT
        fprintf(stderr, "PluginSyncAudioToMain fromAudioQ.try_dequeue: %d %f\n", p.paramIndex,
                p.value);
#endif
        plugin->mainParameters[p.paramIndex] = p.value;
        anyChanged = true;
    }
    return anyChanged;
}

const char *_features[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    NULL,
};

static const clap_plugin_descriptor_t pluginDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
#ifndef CLAP_GUI
    .id = "naren.tuneBfree",
    .name = "tuneBfree",
#else
    .id = "naren.tuneBfreeGUI",
    .name = "tuneBfreeGUI",
#endif
    .vendor = "naren",
    .url = "https://github.com/narenratan/tuneBfree",
    .manual_url = "https://github.com/narenratan/tuneBfree",
    .support_url = "https://github.com/narenratan/tuneBfree",
    .version = "1.0.0",
    .description = "Tonewheel organ with microtuning",

    .features = _features,
};

static const clap_plugin_note_ports_t extensionNotePorts = {
    .count = [](const clap_plugin_t *plugin, bool isInput) -> uint32_t { return isInput ? 1 : 0; },

    .get = [](const clap_plugin_t *plugin, uint32_t index, bool isInput,
              clap_note_port_info_t *info) -> bool {
        if (!isInput || index)
            return false;
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        snprintf(info->name, sizeof(info->name), "%s", "Note Port");
        return true;
    },
};

static const clap_plugin_audio_ports_t extensionAudioPorts = {
    .count = [](const clap_plugin_t *plugin, bool isInput) -> uint32_t { return isInput ? 0 : 1; },

    .get = [](const clap_plugin_t *plugin, uint32_t index, bool isInput,
              clap_audio_port_info_t *info) -> bool {
        if (isInput || index)
            return false;
        info->id = 0;
        info->channel_count = 2;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        snprintf(info->name, sizeof(info->name), "%s", "Audio Output");
        return true;
    },
};

static const clap_plugin_params_t extensionParams = {
    .count = [](const clap_plugin_t *plugin) -> uint32_t { return P_COUNT; },

    .get_info = [](const clap_plugin_t *_plugin, uint32_t index,
                   clap_param_info_t *information) -> bool {
        if ((P_DRAWBAR_MIN <= index) && (index <= P_DRAWBAR_MAX))
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 8.0f;
            float default_drawbar_value[NOF_DRAWBARS] = {7.0, 8.0, 8.0, 0.0};
            information->default_value = default_drawbar_value[index];
            strcpy(information->name, ("Drawbar " + std::to_string(index)).c_str());
            return true;
        }
        else if (index == P_VIBRATO)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 1.0f;
            information->default_value = 0.0f;
            strcpy(information->name, "Vibrato on/off");
            return true;
        }
        else if (index == P_VIBRATO_TYPE)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 5.99f;
            information->default_value = 0.0f;
            strcpy(information->name, "Vibrato type");
            return true;
        }
        else if (index == P_DRUM)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 2.99f;
            information->default_value = 1.0f;
            strcpy(information->name, "Drum");
            return true;
        }
        else if (index == P_HORN)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 2.99f;
            information->default_value = 1.0f;
            strcpy(information->name, "Horn");
            return true;
        }
        else if (index == P_OVERDRIVE)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 1.0f;
            information->default_value = 0.0f;
            strcpy(information->name, "Overdrive on/off");
            return true;
        }
        else if (index == P_CHARACTER)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 1.0f;
            information->default_value = 0.0f;
            strcpy(information->name, "Character");
            return true;
        }
        else if (index == P_REVERB)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 1.0f;
            information->default_value = 0.1f;
            strcpy(information->name, "Reverb wet/dry");
            return true;
        }
        else if (index == P_PERCUSSION)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 1.0f;
            information->default_value = 0.0f;
            strcpy(information->name, "Percussion on/off");
            return true;
        }
        else if (index == P_PERCUSSION_VOLUME)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 1.0f;
            information->default_value = 0.0f;
            strcpy(information->name, "Percussion soft/norm");
            return true;
        }
        else if (index == P_PERCUSSION_DECAY)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 1.0f;
            information->default_value = 0.0f;
            strcpy(information->name, "Percussion fast/slow");
            return true;
        }
        else if (index == P_PERCUSSION_HARMONIC)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 1.0f;
            information->default_value = 0.0f;
            strcpy(information->name, "Percussion 2nd/3rd");
            return true;
        }
        else if ((P_RATIO_TOP_MIN <= index) && (index <= P_RATIO_TOP_MAX))
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 1000.0f;
            float default_ratio_top[NOF_DRAWBARS] = {1, 3, 1, 2, 3, 4, 5, 6, 8};
            information->default_value = default_ratio_top[index - P_RATIO_TOP_MIN];
            strcpy(information->name,
                   ("Ratio top " + std::to_string(index - P_RATIO_TOP_MIN)).c_str());
            return true;
        }
        else if ((P_RATIO_BOTTOM_MIN <= index) && (index <= P_RATIO_BOTTOM_MAX))
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            information->min_value = 0.0f;
            information->max_value = 1000.0f;
            float default_ratio_top[NOF_DRAWBARS] = {2, 2, 1, 1, 1, 1, 1, 1, 1};
            information->default_value = default_ratio_top[index - P_RATIO_BOTTOM_MIN];
            strcpy(information->name,
                   ("Ratio bottom " + std::to_string(index - P_RATIO_BOTTOM_MIN)).c_str());
            return true;
        }
        else
        {
            return false;
        }
    },

    .get_value = [](const clap_plugin_t *_plugin, clap_id id, double *value) -> bool {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        uint32_t i = (uint32_t)id;
        if (i >= P_COUNT)
            return false;
        *value = plugin->parameters[i];
        return true;
    },

    .value_to_text =
        [](const clap_plugin_t *_plugin, clap_id id, double value, char *display, uint32_t size) {
            uint32_t i = (uint32_t)id;
            if (i >= P_COUNT)
                return false;
            snprintf(display, size, "%f", value);
            return true;
        },

    .text_to_value =
        [](const clap_plugin_t *_plugin, clap_id param_id, const char *display, double *value) {
            // TODO Implement this.
            return false;
        },

    .flush =
        [](const clap_plugin_t *_plugin, const clap_input_events_t *in,
           const clap_output_events_t *out) {
            MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
            const uint32_t eventCount = in->size(in);
            PluginSyncMainToAudio(plugin, out);

            for (uint32_t eventIndex = 0; eventIndex < eventCount; eventIndex++)
            {
                PluginProcessEvent(plugin, in->get(in, eventIndex));
            }
        },
};

static const clap_plugin_state_t extensionState = {
    .save = [](const clap_plugin_t *_plugin, const clap_ostream_t *stream) -> bool {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        PluginSyncAudioToMain(plugin);
        return sizeof(double) * P_COUNT ==
               stream->write(stream, plugin->mainParameters, sizeof(double) * P_COUNT);
    },

    .load = [](const clap_plugin_t *_plugin, const clap_istream_t *stream) -> bool {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        bool success = sizeof(double) * P_COUNT ==
                       stream->read(stream, plugin->mainParameters, sizeof(double) * P_COUNT);
        struct ParamMsg p;
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            p = {i, plugin->mainParameters[i]};
#ifdef DEBUG_PRINT
            fprintf(stderr, "extensionState.load toAudioQ.try_enqueue: %d %f\n", p.paramIndex,
                    p.value);
#endif
            toAudioQ.try_enqueue(p);
        }
        return success;
    },
};

#ifdef CLAP_GUI
#include "gui_plumbing.cpp"

constexpr auto bred = ce::colors::red.opacity(0.1);
constexpr auto bblue = ce::colors::light_blue.opacity(0.1);

auto vibrato_controls(double *mainParameters)
{
    auto onOff = ce::toggle_button("On/off", 1.0, bred);
    onOff.value(mainParameters[P_VIBRATO]);
    onOff.on_click = [mainParameters](bool down) {
        mainParameters[P_VIBRATO] = (float)down;
        toAudioQ.try_enqueue({P_VIBRATO, (double)down});
    };

    auto type = ce::dial(ce::basic_knob<50>());
    float scale = 5.99f;
    type.value(mainParameters[P_VIBRATO_TYPE] / scale);
    type.on_change = [mainParameters, scale](double val) {
        double v = scale * val;
        mainParameters[P_VIBRATO_TYPE] = v;
        toAudioQ.try_enqueue({P_VIBRATO_TYPE, v});
    };

    return ce::pane("Vibrato", ce::htile(ce::align_center_middle(ce::fixed_size({50, 50}, onOff)),
                                         ce::align_center(ce::vtile(ce::margin({5, 5, 5, 5}, type),
                                                                    ce::label{"Type"}))));
}

auto percussion_controls(double *mainParameters)
{
    auto onOff = ce::toggle_button("On/off", 1.0, bred);
    onOff.value(mainParameters[P_PERCUSSION]);
    onOff.on_click = [mainParameters](bool down) {
        mainParameters[P_PERCUSSION] = float(down);
        toAudioQ.try_enqueue({P_PERCUSSION, (double)down});
    };

    auto volume = ce::toggle_button("Volume", 1.0, bblue);
    volume.value(mainParameters[P_PERCUSSION_VOLUME]);
    volume.on_click = [mainParameters](bool down) {
        mainParameters[P_PERCUSSION_VOLUME] = (float)down;
        toAudioQ.try_enqueue({P_PERCUSSION_VOLUME, (double)down});
    };

    auto decay = ce::toggle_button("Decay", 1.0, bblue);
    decay.value(mainParameters[P_PERCUSSION_DECAY]);
    decay.on_click = [mainParameters](bool down) {
        mainParameters[P_PERCUSSION_DECAY] = (float)down;
        toAudioQ.try_enqueue({P_PERCUSSION_DECAY, (double)down});
    };

    auto harmonic = ce::toggle_button("Harmonic", 1.0, bblue);
    harmonic.value(mainParameters[P_PERCUSSION_HARMONIC]);
    harmonic.on_click = [mainParameters](bool down) {
        mainParameters[P_PERCUSSION_HARMONIC] = (float)down;
        toAudioQ.try_enqueue({P_PERCUSSION_HARMONIC, (double)down});
    };

    float m = 2.0;
    return ce::pane(
        "Percussion",
        ce::vtile(ce::htile(ce::margin({m, m, m, m}, ce::fixed_size({100, 50}, onOff)),
                            ce::margin({m, m, m, m}, ce::fixed_size({100, 50}, volume))),
                  ce::htile(ce::margin({m, m, m, m}, ce::fixed_size({100, 50}, decay)),
                            ce::margin({m, m, m, m}, ce::fixed_size({100, 50}, harmonic)))));
}

double check(double x)
{
    if (std::isnan(x))
    {
        throw std::runtime_error("nan");
    }
    if (std::isinf(x))
    {
        throw std::runtime_error("inf");
    }
    return x;
}

void parse_ratio(std::string s, double *topOut, double *bottomOut)
{
    int slash;
    slash = s.find('/');
    if (slash != std::string::npos)
    {
        *topOut = std::stoi(s.substr(0, slash));
        *bottomOut = std::stoi(s.substr(slash + 1));
    }
    else
    {
        *topOut = std::stof(s);
        *bottomOut = 1.0;
    }
}

std::string ratioString(double *mainParameters, int i)
{
    int top = (int)mainParameters[P_RATIO_TOP_MIN + i];
    int bottom = (int)mainParameters[P_RATIO_BOTTOM_MIN + i];
    if (bottom == 1)
    {
        return std::to_string(top);
    }
    return std::to_string(top) + "/" + std::to_string(bottom);
}

auto drawbar_controls(double *mainParameters)
{
    auto track = ce::basic_track<5, true>();
    auto marks = ce::slider_marks_lin<40, 8>(track);

    auto slider0 = ce::slider(ce::basic_thumb<25>(), marks);
    auto slider1 = ce::slider(ce::basic_thumb<25>(), marks);
    auto slider2 = ce::slider(ce::basic_thumb<25>(), marks);
    auto slider3 = ce::slider(ce::basic_thumb<25>(), marks);
    auto slider4 = ce::slider(ce::basic_thumb<25>(), marks);
    auto slider5 = ce::slider(ce::basic_thumb<25>(), marks);
    auto slider6 = ce::slider(ce::basic_thumb<25>(), marks);
    auto slider7 = ce::slider(ce::basic_thumb<25>(), marks);
    auto slider8 = ce::slider(ce::basic_thumb<25>(), marks);
    float scale = 8.0f;
    slider0.value(mainParameters[0] / scale);
    slider0.on_change = [mainParameters, scale](double val) {
        double v = scale * val;
        mainParameters[0] = v;
        toAudioQ.try_enqueue({0, v});
    };
    slider1.value(mainParameters[1] / scale);
    slider1.on_change = [mainParameters, scale](double val) {
        double v = scale * val;
        mainParameters[1] = v;
        toAudioQ.try_enqueue({1, v});
    };
    slider2.value(mainParameters[2] / scale);
    slider2.on_change = [mainParameters, scale](double val) {
        double v = scale * val;
        mainParameters[2] = v;
        toAudioQ.try_enqueue({2, v});
    };
    slider3.value(mainParameters[3] / scale);
    slider3.on_change = [mainParameters, scale](double val) {
        double v = scale * val;
        mainParameters[3] = v;
        toAudioQ.try_enqueue({3, v});
    };
    slider4.value(mainParameters[4] / scale);
    slider4.on_change = [mainParameters, scale](double val) {
        double v = scale * val;
        mainParameters[4] = v;
        toAudioQ.try_enqueue({4, v});
    };
    slider5.value(mainParameters[5] / scale);
    slider5.on_change = [mainParameters, scale](double val) {
        double v = scale * val;
        mainParameters[5] = v;
        toAudioQ.try_enqueue({5, v});
    };
    slider6.value(mainParameters[6] / scale);
    slider6.on_change = [mainParameters, scale](double val) {
        double v = scale * val;
        mainParameters[6] = v;
        toAudioQ.try_enqueue({6, v});
    };
    slider7.value(mainParameters[7] / scale);
    slider7.on_change = [mainParameters, scale](double val) {
        double v = scale * val;
        mainParameters[7] = v;
        toAudioQ.try_enqueue({7, v});
    };
    slider8.value(mainParameters[8] / scale);
    slider8.on_change = [mainParameters, scale](double val) {
        double v = scale * val;
        mainParameters[8] = v;
        toAudioQ.try_enqueue({8, v});
    };
    float m = 5.0;

    auto ratio0 = ce::input_box(ratioString(mainParameters, 0));
    ratio0.second->on_text = ON_TEXT(0);
    auto ratio1 = ce::input_box(ratioString(mainParameters, 1));
    ratio1.second->on_text = ON_TEXT(1);
    auto ratio2 = ce::input_box(ratioString(mainParameters, 2));
    ratio2.second->on_text = ON_TEXT(2);
    auto ratio3 = ce::input_box(ratioString(mainParameters, 3));
    ratio3.second->on_text = ON_TEXT(3);
    auto ratio4 = ce::input_box(ratioString(mainParameters, 4));
    ratio4.second->on_text = ON_TEXT(4);
    auto ratio5 = ce::input_box(ratioString(mainParameters, 5));
    ratio5.second->on_text = ON_TEXT(5);
    auto ratio6 = ce::input_box(ratioString(mainParameters, 6));
    ratio6.second->on_text = ON_TEXT(6);
    auto ratio7 = ce::input_box(ratioString(mainParameters, 7));
    ratio7.second->on_text = ON_TEXT(7);
    auto ratio8 = ce::input_box(ratioString(mainParameters, 8));
    ratio8.second->on_text = ON_TEXT(8);

    return ce::pane(
        "Drawbars",
        ce::margin({10, 10, 10, 10}, ce::htile(ce::hmargin(m, ce::vtile(slider0, ratio0.first)),
                                               ce::hmargin(m, ce::vtile(slider1, ratio1.first)),
                                               ce::hmargin(m, ce::vtile(slider2, ratio2.first)),
                                               ce::hmargin(m, ce::vtile(slider3, ratio3.first)),
                                               ce::hmargin(m, ce::vtile(slider4, ratio4.first)),
                                               ce::hmargin(m, ce::vtile(slider5, ratio5.first)),
                                               ce::hmargin(m, ce::vtile(slider6, ratio6.first)),
                                               ce::hmargin(m, ce::vtile(slider7, ratio7.first)),
                                               ce::hmargin(m, ce::vtile(slider8, ratio8.first)))));
}

auto overdrive_controls(double *mainParameters)
{
    auto onOff = ce::toggle_button("On/off", 1.0, bred);
    onOff.value(mainParameters[P_OVERDRIVE]);
    onOff.on_click = [mainParameters](bool down) {
        mainParameters[P_OVERDRIVE] = float(down);
        toAudioQ.try_enqueue({P_OVERDRIVE, (double)down});
    };

    // auto character = ce::dial(ce::radial_marks<20>(ce::basic_knob<25>()));
    auto character = ce::dial(ce::basic_knob<50>());
    character.value(mainParameters[P_CHARACTER]);
    character.on_change = [mainParameters](double val) {
        mainParameters[P_CHARACTER] = val;
        toAudioQ.try_enqueue({P_CHARACTER, val});
    };

    return ce::pane("Overdrive",
                    ce::htile(ce::align_center_middle(ce::fixed_size({50, 50}, onOff)),
                              ce::align_center(ce::vtile(ce::margin({5, 5, 5, 5}, character),
                                                         ce::label{"Character"}))));
}

auto reverb_controls(double *mainParameters)
{
    auto wetDry = ce::dial(ce::basic_knob<50>());
    wetDry.value(mainParameters[P_REVERB]);
    wetDry.on_change = [mainParameters](double val) {
        mainParameters[P_REVERB] = val;
        toAudioQ.try_enqueue({P_REVERB, val});
    };
    return ce::pane("Reverb", ce::align_center(ce::margin(
                                  {5, 5, 5, 5}, ce::vtile(ce::margin({5, 5, 5, 5}, wetDry),
                                                          ce::label{"Wet/dry"}))));
}

auto leslie_controls(double *mainParameters)
{
    float scale = 2.99f;

    auto drum = ce::dial(ce::basic_knob<50>());
    drum.value(mainParameters[P_DRUM] / scale);
    drum.on_change = [scale, mainParameters](double val) {
        double v = scale * val;
        mainParameters[P_DRUM] = v;
        toAudioQ.try_enqueue({P_DRUM, v});
    };

    auto horn = ce::dial(ce::basic_knob<50>());
    horn.value(mainParameters[P_HORN] / scale);
    horn.on_change = [scale, mainParameters](double val) {
        double v = scale * val;
        mainParameters[P_HORN] = v;
        toAudioQ.try_enqueue({P_HORN, v});
    };

    return ce::pane(
        "Leslie",
        ce::align_center(ce::htile(
            ce::align_center(ce::vtile(ce::margin({5, 5, 5, 5}, drum), ce::label{"Drum"})),
            ce::align_center(ce::vtile(ce::margin({5, 5, 5, 5}, horn), ce::label{"Horn"})))));
}

void GUISetup(MyPlugin *plugin)
{
    float m = 5.0;
    plugin->gui->view->content(ce::align_center_middle(ce::pane(
        ce::label("tuneBfree").font_size(18),
        ce::htile(ce::margin({m, m, m, m}, drawbar_controls(plugin->mainParameters)),
                  ce::vtile(ce::margin({m, m, m, m}, percussion_controls(plugin->mainParameters)),
                            ce::margin({m, m, m, m}, vibrato_controls(plugin->mainParameters)),
                            ce::margin({m, m, m, m}, overdrive_controls(plugin->mainParameters)),
                            ce::margin({m, m, m, m}, reverb_controls(plugin->mainParameters)),
                            ce::margin({m, m, m, m}, leslie_controls(plugin->mainParameters)))))));

#if defined(_WIN32)
    plugin->gui->window = plugin->gui->view->host();
#elif defined(__linux__)
    plugin->gui->display = ce::get_display();
    plugin->gui->window = plugin->gui->view->host()->x_window;

    if (plugin->hostPOSIXFDSupport && plugin->hostPOSIXFDSupport->register_fd)
    {
        plugin->hostPOSIXFDSupport->register_fd(
            plugin->host, ConnectionNumber(plugin->gui->display), CLAP_POSIX_FD_READ);
    }
#endif
}

static const clap_plugin_gui_t extensionGUI = {
    .is_api_supported = [](const clap_plugin_t *plugin, const char *api, bool isFloating) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.is_api_supported\n");
#endif
        return 0 == strcmp(api, GUI_API) && !isFloating;
    },

    .get_preferred_api = [](const clap_plugin_t *plugin, const char **api,
                            bool *isFloating) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.get_preferred_api\n");
#endif
        *api = GUI_API;
        *isFloating = false;
        return true;
    },

    .create = [](const clap_plugin_t *_plugin, const char *api, bool isFloating) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.create\n");
#endif
        if (!extensionGUI.is_api_supported(_plugin, api, isFloating))
            return false;
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        GUICreate(plugin);
        GUISetup(plugin);
        return true;
    },

    .destroy =
        [](const clap_plugin_t *_plugin) {
#ifdef DEBUG_PRINT
            fprintf(stderr, "extensionGUI.destroy\n");
#endif
            GUIDestroy((MyPlugin *)_plugin->plugin_data);
        },

    .set_scale = [](const clap_plugin_t *plugin, double scale) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.set_scale\n");
#endif
        return false;
    },

    .get_size = [](const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.get_size\n");
#endif
        *width = GUI_WIDTH;
        *height = GUI_HEIGHT;
        return true;
    },

    .can_resize = [](const clap_plugin_t *plugin) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.can_resize\n");
#endif
        return false;
    },

    .get_resize_hints = [](const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.get_resize_hints\n");
#endif
        return false;
    },

    .adjust_size = [](const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.adjust_size\n");
#endif
        return extensionGUI.get_size(plugin, width, height);
    },

    .set_size = [](const clap_plugin_t *plugin, uint32_t width, uint32_t height) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.set_size\n");
#endif
        return true;
    },

    .set_parent = [](const clap_plugin_t *_plugin, const clap_window_t *window) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.set_parent\n");
#endif
        assert(0 == strcmp(window->api, GUI_API));
        GUISetParent((MyPlugin *)_plugin->plugin_data, window);
        return true;
    },

    .set_transient = [](const clap_plugin_t *plugin, const clap_window_t *window) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.set_transient\n");
#endif
        return false;
    },

    .suggest_title =
        [](const clap_plugin_t *plugin, const char *title) {
#ifdef DEBUG_PRINT
            fprintf(stderr, "extensionGUI.suggest_title\n");
#endif
        },

    .show = [](const clap_plugin_t *_plugin) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.show\n");
#endif
        GUISetVisible((MyPlugin *)_plugin->plugin_data, true);
        return true;
    },

    .hide = [](const clap_plugin_t *_plugin) -> bool {
#ifdef DEBUG_PRINT
        fprintf(stderr, "extensionGUI.hide\n");
#endif
        GUISetVisible((MyPlugin *)_plugin->plugin_data, false);
        return true;
    },
};

static const clap_plugin_posix_fd_support_t extensionPOSIXFDSupport = {
    .on_fd =
        [](const clap_plugin_t *_plugin, int fd, clap_posix_fd_flags_t flags) {
#ifdef DEBUG_PRINT
            fprintf(stderr, "extensionPOSIXFDSupport.on_fd\n");
#endif
            MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
            GUIOnPOSIXFD(plugin);
        },
};
#endif // CLAP_GUI

static const clap_plugin_t pluginClass = {
    .desc = &pluginDescriptor,
    .plugin_data = nullptr,

    .init = [](const clap_plugin *_plugin) -> bool {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;

#ifdef CLAP_GUI
        plugin->hostPOSIXFDSupport =
            (const clap_host_posix_fd_support_t *)plugin->host->get_extension(
                plugin->host, CLAP_EXT_POSIX_FD_SUPPORT);
#endif
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            clap_param_info_t information = {};
            extensionParams.get_info(_plugin, i, &information);
            plugin->mainParameters[i] = plugin->parameters[i] = information.default_value;
        }
        return true;
    },

    .destroy =
        [](const clap_plugin *_plugin) {
            MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
            if (plugin->synth)
            {
                freeToneGenerator(plugin->synth);
            }
            if (plugin->preamp)
            {
                freePreamp((void *)plugin->preamp);
            }
            if (plugin->reverb)
            {
                freeReverb(plugin->reverb);
            }
            if (plugin->whirl)
            {
                freeWhirl(plugin->whirl);
            }
            if (plugin->client)
            {
                MTS_DeregisterClient(plugin->client);
            }
            free(plugin);
        },

    .activate = [](const clap_plugin *_plugin, double sampleRate, uint32_t minimumFramesCount,
                   uint32_t maximumFramesCount) -> bool {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        plugin->sampleRate = sampleRate;

        plugin->synth = allocTonegen();
        double targetRatio[NOF_DRAWBARS];
        for (int i = 0; i < NOF_DRAWBARS; i++)
        {
            targetRatio[i] = getRatio(plugin->parameters, i);
        }
        initToneGenerator(plugin->synth, nullptr, sampleRate, targetRatio);
        init_vibrato(&(plugin->synth->inst_vibrato), sampleRate);
        plugin->whirl = allocWhirl();
        initWhirl(plugin->whirl, nullptr, sampleRate);
        void *preamp = allocPreamp();
        plugin->preamp = (struct b_preamp *)preamp;
        initPreamp(preamp, nullptr, sampleRate);
        plugin->reverb = allocReverb();
        initReverb(plugin->reverb, nullptr, sampleRate);
        plugin->client = MTS_RegisterClient();

        // Apply current parameter values to newly created DSP objects
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            setParam(plugin, i, plugin->parameters[i]);
        }

        for (int i = 0; i < NOF_DRAWBARS; i++)
        {
            plugin->previousRatio[i] = targetRatio[i];
        }
        return true;
    },

    .deactivate = [](const clap_plugin *_plugin) {},

    .start_processing = [](const clap_plugin *_plugin) -> bool { return true; },

    .stop_processing = [](const clap_plugin *_plugin) {},

    .reset =
        [](const clap_plugin *_plugin) { MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data; },

    .process = [](const clap_plugin *_plugin,
                  const clap_process_t *process) -> clap_process_status {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;

        int i;
        bool tuningChanged = false;
        double newFrequency[128];

        for (i = 0; i < 128; i++)
        {
            newFrequency[i] = MTS_NoteToFrequency(plugin->client, i, 0);
            if (newFrequency[i] != plugin->previousFrequency[i])
            {
                tuningChanged = true;
            }
        }

        if (tuningChanged)
        {
#ifdef DEBUG_PRINT
            fprintf(stderr, "Detected MTS-ESP tuning change\n");
#endif
            reinitToneGen(plugin);
            for (i = 0; i < 128; i++)
            {
                plugin->previousFrequency[i] = newFrequency[i];
            }
        }

        bool ratioChanged = false;
        double newRatio[NOF_DRAWBARS];
        for (i = 0; i < NOF_DRAWBARS; i++)
        {
            newRatio[i] = getRatio(plugin->parameters, i);
            if (newRatio[i] != plugin->previousRatio[i])
            {
                ratioChanged = true;
            }
        }

        if (ratioChanged)
        {
#ifdef DEBUG_PRINT
            fprintf(stderr, "Detected drawbar ratio change\n");
#endif
            reinitToneGen(plugin);
            for (i = 0; i < NOF_DRAWBARS; i++)
            {
                plugin->previousRatio[i] = newRatio[i];
            }
        }

        assert(process->audio_outputs_count == 1);
        assert(process->audio_inputs_count == 0);

        const uint32_t frameCount = process->frames_count;
        const uint32_t inputEventCount = process->in_events->size(process->in_events);
        uint32_t eventIndex = 0;
        uint32_t nextEventFrame = inputEventCount ? 0 : frameCount;

        PluginSyncMainToAudio(plugin, process->out_events);

        for (uint32_t i = 0; i < frameCount;)
        {
            while (eventIndex < inputEventCount && nextEventFrame == i)
            {
                const clap_event_header_t *event =
                    process->in_events->get(process->in_events, eventIndex);

                if (event->time != i)
                {
                    nextEventFrame = event->time;
                    break;
                }

                PluginProcessEvent(plugin, event);
                eventIndex++;

                if (eventIndex == inputEventCount)
                {
                    nextEventFrame = frameCount;
                    break;
                }
            }

            PluginRenderAudio(plugin, i, nextEventFrame, process->audio_outputs[0].data32[0],
                              process->audio_outputs[0].data32[1]);
            i = nextEventFrame;
        }

        return CLAP_PROCESS_CONTINUE;
    },

    .get_extension = [](const clap_plugin *plugin, const char *id) -> const void * {
        if (0 == strcmp(id, CLAP_EXT_NOTE_PORTS))
            return &extensionNotePorts;
        if (0 == strcmp(id, CLAP_EXT_AUDIO_PORTS))
            return &extensionAudioPorts;
        if (0 == strcmp(id, CLAP_EXT_PARAMS))
            return &extensionParams;
#ifdef CLAP_GUI
        if (0 == strcmp(id, CLAP_EXT_GUI))
            return &extensionGUI;
        if (0 == strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
            return &extensionPOSIXFDSupport;
#endif
        if (0 == strcmp(id, CLAP_EXT_STATE))
            return &extensionState;
        return nullptr;
    },

    .on_main_thread = [](const clap_plugin *_plugin) {},
};

static const clap_plugin_factory_t pluginFactory = {
    .get_plugin_count = [](const clap_plugin_factory *factory) -> uint32_t { return 1; },

    .get_plugin_descriptor = [](const clap_plugin_factory *factory, uint32_t index)
        -> const clap_plugin_descriptor_t * { return index == 0 ? &pluginDescriptor : nullptr; },

    .create_plugin = [](const clap_plugin_factory *factory, const clap_host_t *host,
                        const char *pluginID) -> const clap_plugin_t * {
        if (!clap_version_is_compatible(host->clap_version) ||
            strcmp(pluginID, pluginDescriptor.id))
        {
            return nullptr;
        }

        MyPlugin *plugin = (MyPlugin *)calloc(1, sizeof(MyPlugin));
        plugin->host = host;
        plugin->plugin = pluginClass;
        plugin->plugin.plugin_data = plugin;
        return &plugin->plugin;
    },
};

extern "C" const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,

    .init = [](const char *path) -> bool { return true; },

    .deinit = []() {},

    .get_factory = [](const char *factoryID) -> const void * {
        return strcmp(factoryID, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &pluginFactory;
    },
};
