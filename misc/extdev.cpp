#include "extdev.h"
#include <cmath>
#include <string>
#include <windows.h>
#include "util/utils.h"
#include "util/detour.h"
#include "util/libutils.h"
#include "util/circular_buffer.h"
#include "misc/eamuse.h"
#include "cfg/api.h"
#include "acio/icca/icca.h"
#include "games/gitadora/io.h"
#include "avs/game.h"

using namespace GameAPI;

// card unit
static size_t EXTDEV_CARDUNIT_COUNT = 0;
static circular_buffer<int> EXTDEV_CARDUNIT_KEY[2] = {
        circular_buffer<int>(32),
        circular_buffer<int>(32)
};
static circular_buffer<const char*> EXTDEV_CARDUNIT_KEY_STR[2] = {
        circular_buffer<const char*>(32),
        circular_buffer<const char*>(32)
};
static int EXTDEV_CARDUNIT_EJECT[2] = {0, 0};
static bool EXTDEV_CARD_IN[2] = {false, false};
static bool EXTDEV_CARD_PRESSED[2] = {false, false};
static uint8_t EXTDEV_CARD[2][8];
static bool EXTDEV_CARDUNIT_TENKEY_STATE[2][12]{};
static std::string EXTDEV_CARDUNIT_TENKEY_STRINGS[] = {
        "0",
        "1",
        "2",
        "3",
        "4",
        "5",
        "6",
        "7",
        "8",
        "9",
        "RETURN", // enter
        "00" // double zero
};
static unsigned int EXTDEV_CARDUNIT_TENKEY_EAMUSE_MAPPING[] = {
        0, 1, 5, 9, 2, 6, 10, 3, 7, 11, 8, 4
};
static unsigned int EXTDEV_CARDUNIT_TENKEY_NUMS[] {
        0, 1, 5, 9, 2, 6, 10, 3, 7, 11, 8, 4
};

// GFDM
static int32_t GFDM_DM_ATTACK_BORDER_VALUE[] = {
        0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50
};
static bool GFDM_GF_PICK_STATE_UP[2]{};
static bool GFDM_GF_PICK_STATE_DOWN[2]{};

// state
static HINSTANCE EXTDEV_INSTANCE;
static std::string EXTDEV_INSTANCE_NAMES[] = {
                "ext_dev.dll",
                "libextio.dll",
                "libcardunit.dll",
                "libledunit.dll",
                "libgfdm_unit2.dll",
};

static void __cdecl cardunit_boot2(int a1, int a2, int a3) {

    // default reader count to 1
    EXTDEV_CARDUNIT_COUNT = 1;

    // exceptions for games with two readers
    if (avs::game::is_model({ "J33", "K33", "L33", "M32" })) {
        EXTDEV_CARDUNIT_COUNT = 2;
    }
}

static void __cdecl cardunit_boot(int a1, int a2) {
    cardunit_boot2(1, a1, a2);
}

static int __cdecl cardunit_boot_initialize() {
    return 0;
}

static void __cdecl cardunit_boot_no_slot_type(int a1, int a2) {
    cardunit_boot2(1, a1, a2);
}

static void __cdecl cardunit_card_eject(int unit) {
    EXTDEV_CARDUNIT_EJECT[unit] = 1;
}

static int __cdecl cardunit_card_eject_complete(int unit) {
    EXTDEV_CARD_IN[unit] = false;
    return EXTDEV_CARDUNIT_EJECT[unit];
}

static int __cdecl cardunit_card_eject_wait(int unit) {
    return EXTDEV_CARDUNIT_EJECT[unit];
}

static int __cdecl cardunit_card_read2(int unit, void *card, int *status) {

    // clear the eject flag
    EXTDEV_CARDUNIT_EJECT[unit] = 0;

    // check if a card was inserted
    if (EXTDEV_CARD_IN[unit]) {

        // copy card, return success
        memcpy(card, EXTDEV_CARD[unit], 8);
        *status = is_card_uid_felica(EXTDEV_CARD[unit]) ? 2 : 1;
        return 0;

    } else {

        // tried to read card with no card inserted, return fail
        memset(card, 0, 8);
        *status = 0;
        return 1;
    }
}

static int __cdecl cardunit_card_read(int unit, void *card) {
    return cardunit_card_read2(unit, card, nullptr);
}

static void __cdecl cardunit_card_ready(int unit) {
    EXTDEV_CARDUNIT_EJECT[unit] = 0;
}

static int __cdecl cardunit_card_sensor() {
    return 0;
}

static int __cdecl cardunit_card_sensor_raw(int a1) {
    return 1;
}

static void __cdecl cardunit_update() {

    // update all units
    for (size_t unit = 0; unit < EXTDEV_CARDUNIT_COUNT; unit++) {
        bool kb_insert_press = false;

        // eamio keypress
        kb_insert_press |= eamuse_get_keypad_state(unit) & (1 << EAM_IO_INSERT);

        // update card inserts
        if (eamuse_card_insert_consume((int) EXTDEV_CARDUNIT_COUNT, unit) ||
                (kb_insert_press && !EXTDEV_CARD_PRESSED[unit])) {
            EXTDEV_CARD_PRESSED[unit] = true;
            if (!EXTDEV_CARD_IN[unit]) {
                EXTDEV_CARD_IN[unit] = true;
                eamuse_get_card((int) EXTDEV_CARDUNIT_COUNT, unit, EXTDEV_CARD[unit]);
            }
        } else
            EXTDEV_CARD_PRESSED[unit] = false;

        // get eamu key states
        uint16_t eamu_state = eamuse_get_keypad_state(unit);

        // iterate all keys
        for (int i = 0; i < 12; i++) {

            // check if key is pressed
            if (eamu_state & (1 << EXTDEV_CARDUNIT_TENKEY_EAMUSE_MAPPING[i])) {

                // check if key was pressed before
                if (!EXTDEV_CARDUNIT_TENKEY_STATE[unit][i]) {

                    // remember key press
                    EXTDEV_CARDUNIT_TENKEY_STATE[unit][i] = true;

                    // set last key
                    EXTDEV_CARDUNIT_KEY[unit].put(EXTDEV_CARDUNIT_TENKEY_NUMS[i]);
                    EXTDEV_CARDUNIT_KEY_STR[unit].put(EXTDEV_CARDUNIT_TENKEY_STRINGS[i].c_str());

                    // we can only detect one key at a time
                    break;
                }

            } else {

                // forget old key press
                EXTDEV_CARDUNIT_TENKEY_STATE[unit][i] = false;
            }
        }
    }
}

static int __cdecl cardunit_get_status(int unit) {

    // might not be needed
    cardunit_update();

    // TODO: why only for reflec beat?
    if (avs::game::is_model("MBR")) {
        return EXTDEV_CARD_IN[unit] && is_card_uid_felica(EXTDEV_CARD[unit]) ? 2 : 1;
    }

    // gitadora always wants 1 apparently
    return 1;
}

static long __cdecl cardunit_cardnumber_obfuscate_decode(void* a1, int a2, void* a3, void* a4) {
    return 0;
}

static long __cdecl cardunit_cardnumber_obfuscate_encode(void* a1, int a2, void* a3, void* a4) {
    return 0;
}

static int __cdecl cardunit_get_errorcount(int a1) {
    return 0;
}

static int __cdecl cardunit_get_recvcount(int a1) {
    return 0;
}

static int __cdecl cardunit_get_sendcount(int a1) {
    return 0;
}

static const char* __cdecl cardunit_get_version(int a1) {
    static const char* ver = "DUMMY\x04\x02\x00";
    return ver;
}

static int __cdecl cardunit_check_version() {
    return 1;
}

static int __cdecl cardunit_key_get(int unit) {
    if (EXTDEV_CARDUNIT_KEY[unit].empty())
        return -1;
    return EXTDEV_CARDUNIT_KEY[unit].get();
}

static const char *__cdecl cardunit_key_str(int unit) {
    if (EXTDEV_CARDUNIT_KEY_STR[unit].empty())
        return nullptr;
    return EXTDEV_CARDUNIT_KEY_STR[unit].get();
}

static int __cdecl cardunit_reset() {
    return 0;
}

static void __cdecl cardunit_shutdown() {
}

static void __cdecl cardunit_sleep(int unit) {
}

static const size_t gitadora_button_mapping[] = {
        games::gitadora::Buttons::Service,
        games::gitadora::Buttons::Test,
        games::gitadora::Buttons::Coin,
        games::gitadora::Buttons::GuitarP1PickUp,
        games::gitadora::Buttons::GuitarP1PickDown,
        games::gitadora::Buttons::GuitarP1R,
        games::gitadora::Buttons::GuitarP1G,
        games::gitadora::Buttons::GuitarP1B,
        games::gitadora::Buttons::GuitarP1Y,
        games::gitadora::Buttons::GuitarP1P,
        games::gitadora::Buttons::GuitarP1KnobUp,
        games::gitadora::Buttons::GuitarP1KnobDown,
        games::gitadora::Buttons::GuitarP1WailUp,
        games::gitadora::Buttons::GuitarP1WailDown,
        games::gitadora::Buttons::GuitarP2PickUp,
        games::gitadora::Buttons::GuitarP2PickDown,
        games::gitadora::Buttons::GuitarP2R,
        games::gitadora::Buttons::GuitarP2G,
        games::gitadora::Buttons::GuitarP2B,
        games::gitadora::Buttons::GuitarP2Y,
        games::gitadora::Buttons::GuitarP2P,
        games::gitadora::Buttons::GuitarP2KnobUp,
        games::gitadora::Buttons::GuitarP2KnobDown,
        games::gitadora::Buttons::GuitarP2WailUp,
        games::gitadora::Buttons::GuitarP2WailDown,
        games::gitadora::Buttons::DrumHiHat,
        games::gitadora::Buttons::DrumHiHatClosed,
        games::gitadora::Buttons::DrumHiHatHalfOpen,
        games::gitadora::Buttons::DrumSnare,
        games::gitadora::Buttons::DrumHiTom,
        games::gitadora::Buttons::DrumLowTom,
        games::gitadora::Buttons::DrumRightCymbal,
        games::gitadora::Buttons::DrumBassPedal,
        games::gitadora::Buttons::DrumLeftCymbal,
        games::gitadora::Buttons::DrumLeftPedal,
        games::gitadora::Buttons::DrumFloorTom,
};

static const size_t gitadora_analog_mapping[] = {
        games::gitadora::Analogs::GuitarP1WailX,
        games::gitadora::Analogs::GuitarP1WailY,
        games::gitadora::Analogs::GuitarP1WailZ,
        games::gitadora::Analogs::GuitarP1Knob,
        games::gitadora::Analogs::GuitarP2WailX,
        games::gitadora::Analogs::GuitarP2WailY,
        games::gitadora::Analogs::GuitarP2WailZ,
        games::gitadora::Analogs::GuitarP2Knob,
};

static void __cdecl gfdm_unit_boot(unsigned int is_dm, unsigned int a2, unsigned int a3) {
}

static void __cdecl gfdm_unit2_boot(unsigned int is_dm, unsigned int a2, unsigned int a3) {
}

static int __cdecl gfdm_unit_boot_initialize() {
    return 1;
}

static int __cdecl gfdm_unit2_boot_initialize() {
    return 1;
}

static void *__cdecl gfdm_unit_get_button_p(void *a1, int a2, size_t player) {
    memset(a1, 0, 64);

    // irrelevant for drummania
    if (avs::game::is_model({ "J32", "K32", "L32" }) ||
             (avs::game::is_model("M32") && avs::game::SPEC[0] == 'B'))
        return a1;

    // get buttons
    auto &buttons = games::gitadora::get_buttons();
    auto &analogs = games::gitadora::get_analogs();

    // X
    ((int *) a1)[4] = a2 == 1 ? 4080 : -4080;
    if (analogs.at(player * 3 + 0).isSet())
        ((int *) a1)[4] = lroundf(Analogs::getState(
                RI_MGR, analogs.at(gitadora_analog_mapping[player * 4 + 0])) * 8160.f) - 4080;

    // Y
    ((int *) a1)[5] = 0;
    if (analogs.at(player * 3 + 1).isSet())
        ((int *) a1)[5] = lroundf(Analogs::getState(
                RI_MGR, analogs.at(gitadora_analog_mapping[player * 4 + 1])) * 8160.f) - 4080;
    if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[12 + 11 * (size_t) player])))
        ((int *) a1)[5] = -4080;
    if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[13 + 11 * (size_t) player])))
        ((int *) a1)[5] = 4080;

    // Z
    ((int *) a1)[6] = 0;
    if (analogs.at(player * 3 + 2).isSet())
        ((int *) a1)[6] = lroundf(Analogs::getState(
                RI_MGR, analogs.at(gitadora_analog_mapping[player * 4 + 2])) * 8160.f) - 4080;

    // return the same buffer
    return a1;
}

static void *__cdecl gfdm_unit_get_button(void *a1, int a2) {
    return gfdm_unit_get_button_p(a1, a2, 0);
}

static void *__cdecl gfdm_unit2_get_button(void *a1, int a2) {
    return gfdm_unit_get_button_p(a1, a2, 1);
}

static bool __cdecl gfdm_unit_get_button_dm(int a1) {
    return a1 == 9 || a1 == 8;
}

static bool __cdecl gfdm_unit2_get_button_dm(int a1) {
    return a1 == 9 || a1 == 8;
}

static int __cdecl gfdm_unit_get_button_gf(int a1, int a2) {
    return 0;
}

static int __cdecl gfdm_unit2_get_button_gf(int a1, int a2) {
    return 0;
}

static long __cdecl gfdm_unit_get_dm_attack_border_value(int a1, int *a2) {
    if (a1 >= 7)
        return -1;
    memcpy(a2, GFDM_DM_ATTACK_BORDER_VALUE, 7 * sizeof(int32_t));
    return 0;
}

static long __cdecl gfdm_unit2_get_dm_attack_border_value(int a1, int *a2) {
    if (a1 >= 7)
        return -1;
    memcpy(a2, GFDM_DM_ATTACK_BORDER_VALUE, 7 * sizeof(int32_t));
    return 0;
}

static long __cdecl gfdm_unit_get_errorcount(int a1) {
    return 0;
}

static long __cdecl gfdm_unit2_get_errorcount(int a1) {
    return 0;
}

static long __cdecl gfdm_unit_get_input_p(int device, size_t player) {
    long ret = 0;

    // get buttons and analogs
    auto &buttons = games::gitadora::get_buttons();
    auto &analogs = games::gitadora::get_analogs();

    // drum mania
    if (avs::game::is_model({ "J32", "K32", "L32" }) ||
             (avs::game::is_model("M32") && avs::game::SPEC[0] == 'B')) {

        // we don't want input for this
        if (device == 1)
            return 0;

        // hi hat
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[25])) ||
                Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[26])) ||
                Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[27])))
        {
            ret |= 0x20;
        }

        // snare
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[28]))) {
            ret |= 0x40;
        }

        // hi tom
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[29]))) {
            ret |= 0x80;
        }

        // low tom
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[30]))) {
            ret |= 0x100;
        }

        // right cymbal
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[31]))) {
            ret |= 0x200;
        }

        // bass pedal
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[32]))) {
            ret |= 0x800;
        }

        // left cymbal
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[33]))) {
            ret |= 0x4000;
        }

        // left pedal
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[34]))) {
            ret |= 0x8000;
        }

        // floor tom
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[35]))) {
            ret |= 0x10000;
        }
    }

    // guitar freaks
    if (avs::game::is_model({ "J33", "K33", "L33" }) ||
             (avs::game::is_model("M32") && avs::game::SPEC[0] == 'A'))
    {
        auto offset = player * 11;

        // pick up
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[3 + offset]))) {
            if (!GFDM_GF_PICK_STATE_UP[player]) {
                GFDM_GF_PICK_STATE_UP[player] = true;
                ret |= 0x80 | 0x20;
            }
        } else {
            GFDM_GF_PICK_STATE_UP[player] = false;
        }

        // pick down
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[4 + offset]))) {
            if (!GFDM_GF_PICK_STATE_DOWN[player]) {
                GFDM_GF_PICK_STATE_DOWN[player] = true;
                ret |= 0x100 | 0x20;
            }
        } else {
            GFDM_GF_PICK_STATE_DOWN[player] = false;
        }

        // button R
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[5 + offset]))) {
            ret |= 0x200;
        }

        // button G
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[6 + offset]))) {
            ret |= 0x400;
        }

        // button B
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[7 + offset]))) {
            ret |= 0x800;
        }

        // button Y
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[8 + offset]))) {
            ret |= 0x1000;
        }

        // button P
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[9 + offset]))) {
            ret |= 0x4000;
        }

        // knob statics
        static size_t knob[2]{};
        static long knob_flags[] = {
                0x8000,
                0x18000,
                0x10000,
                0x30000,
                0x38000,
                0x28000
        };

        // knob up
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[10 + offset]))) {
            if ((knob[player] >> 2) < 5) {
                knob[player]++;
            }
        }

        // knob down
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[11 + offset]))) {
            if (knob[player] > 0) {
                knob[player]--;
            }
        }

        // get value from 0 to 5
        size_t value = knob[player] >> 2;

        // analog override
        if (analogs.at(gitadora_analog_mapping[player * 4 + 3]).isSet()) {
            value = (size_t) (Analogs::getState(
                    RI_MGR, analogs.at(gitadora_analog_mapping[player * 4 + 3])) * 5.999f);
        }

        // apply value
        ret |= knob_flags[value];
    }

    return ret;
}

static long __cdecl gfdm_unit_get_input(int device) {
    if (device > 0) {
        return gfdm_unit_get_input_p(device, 1);
    } else {
        return gfdm_unit_get_input_p(device, 0);
    }
}

static long __cdecl gfdm_unit2_get_input(int device) {
    return gfdm_unit_get_input_p(device, 1);
}

static long __cdecl gfdm_unit_get_sensor_gf_p(int a1, int a2, size_t player) {

    // return if it's actually drum mania
    if (avs::game::is_model({ "J32", "K32", "L32" }) ||
             (avs::game::is_model("M32") && avs::game::SPEC[0] == 'B'))
        return 0;

    // get buttons and analogs
    auto &buttons = games::gitadora::get_buttons();
    auto &analogs = games::gitadora::get_analogs();

    // X
    if (a2 == 0) {

        // analog override
        if (analogs.at(gitadora_analog_mapping[player * 3 + 0]).isSet()) {
            return lroundf(Analogs::getState(
                    RI_MGR, analogs.at(gitadora_analog_mapping[player * 4 + 0])) * 8160.f) - 4080;
        }

        // default
        return a1 == 1 ? 4080 : -4080;
    }

    // Y
    if (a2 == 1) {

        // analog override
        if (analogs.at(player * 3 + 1).isSet()) {
            return lroundf(Analogs::getState(
                    RI_MGR, analogs.at(gitadora_analog_mapping[player * 4 + 1])) * 8160.f) - 4080;
        }

        // variables
        long ret = 0;
        size_t offset = (size_t) player * 11;

        // wail up
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[12 + offset]))) {
            ret -= 4080;
        }

        // wail down
        if (Buttons::getState(RI_MGR, buttons.at(gitadora_button_mapping[13 + offset]))) {
            ret += 4080;
        }

        // return value
        return ret;
    }

    // Z
    if (a2 == 2) {

        // analog override
        if (analogs.at(gitadora_analog_mapping[player * 3 + 2]).isSet()) {
            return lroundf(Analogs::getState(
                    RI_MGR, analogs.at(gitadora_analog_mapping[player * 4 + 2])) * 8160.f) - 4080;
        }

        // default
        return 0;
    }

    // unknown sensor
    return 0;
}

static long __cdecl gfdm_unit_get_sensor_gf(int a1, int a2) {
    return gfdm_unit_get_sensor_gf_p(a1, a2, 0);
}

static long __cdecl gfdm_unit2_get_sensor_gf(int a1, int a2) {
    return gfdm_unit_get_sensor_gf_p(a1, a2, 1);
}

static int __cdecl gfdm_unit_get_status(int a1) {
    return 1;
}

static int __cdecl gfdm_unit2_get_status(int a1) {
    return 1;
}

static int __cdecl gfdm_unit_get_stream_errorcount(int a1) {
    return 0;
}

static int __cdecl gfdm_unit2_get_stream_errorcount(int a1) {
    return 0;
}

static const char *__cdecl gfdm_unit_get_version() {
    static const char* ver = "DUMMY\x04\x02\x00";
    return ver;
}

static const char *__cdecl gfdm_unit2_get_version() {
    static const char* ver = "DUMMY\x04\x02\x00";
    return ver;
}

static long __cdecl gfdm_unit_reset() {
    return 0;
}

static long __cdecl gfdm_unit2_reset() {
    return 0;
}

static void __cdecl gfdm_unit_reset_stream_errorcount(int a1) {
}

static void __cdecl gfdm_unit2_reset_stream_errorcount(int a1) {
}

static int __cdecl gfdm_unit_send_motor_value(size_t motor, uint8_t value) {
    if (motor == 0) {
        auto &lights = games::gitadora::get_lights();
        auto &light = lights.at(games::gitadora::Lights::GuitarP1Motor);
        GameAPI::Lights::writeLight(RI_MGR, light, value / 127.f);
    }
    return 0;
}

static int __cdecl gfdm_unit2_send_motor_value(size_t motor, uint8_t value) {
    if (motor == 0) {
        auto &lights = games::gitadora::get_lights();
        auto &light = lights.at(games::gitadora::Lights::GuitarP2Motor);
        GameAPI::Lights::writeLight(RI_MGR, light, value / 127.f);
    }
    return 0;
}

static void __cdecl gfdm_unit_set_dm_attack_border_value(int32_t *a1) {
    memcpy(GFDM_DM_ATTACK_BORDER_VALUE, a1, 7 * sizeof(int32_t));
}

static void __cdecl gfdm_unit2_set_dm_attack_border_value(int32_t *a1) {
    memcpy(GFDM_DM_ATTACK_BORDER_VALUE, a1, 7 * sizeof(int32_t));
}

static void __cdecl gfdm_unit_shutdown() {
}

static void __cdecl gfdm_unit2_shutdown() {
}

static void __cdecl gfdm_unit_update() {
}

static void __cdecl gfdm_unit2_update() {
}

static const char* __cdecl led_get_version() {
    static const char* ver = "DUMMY\x04\x02\x00";
    return ver;
}

static void __cdecl led_unit_boot(int a1, int a2) {
}

static int __cdecl led_unit_boot_initialize() {
    return 0;
}

static int __cdecl led_unit_get_errorcount(int a1) {
    return 0;
}

static int __cdecl led_unit_get_status() {
    return 1;
}

static int __cdecl led_unit_reset() {
    return 0;
}

static unsigned int __cdecl led_unit_send_custom1(int a1, unsigned char *a2) {
    return 0;
}

static unsigned int __cdecl led_unit_send_custom2(int a1, unsigned char *a2) {
    return 0;
}

static long __cdecl led_unit_send_direct(long a1, uint8_t *a2) {
    return 0;
}

static void __cdecl led_unit_shutdown() {
}

static void __cdecl led_unit_update() {
}

static int __cdecl sci_boot() {
    return 1;
}

static int __cdecl sci_clear_error(int a1) {
    return 0;
}

static int __cdecl sci_close(int a1) {
    return 0;
}

static int __cdecl sci_flush() {
    return 0;
}

static int __cdecl sci_flush_complete() {
    return 1;
}

static int __cdecl sci_get_error(int a1) {
    return 0;
}

static int __cdecl sci_gets(int a1, int a2, int a3) {
    return 0;
}

static int __cdecl sci_print_error(int a1, void *a2) {
    return 0;
}

static int __cdecl sci_puts(int a1, int a2, int a3) {
    return 0;
}

static int __cdecl sci_set_linebreak(int a1, int a2) {
    return 0;
}

static int __cdecl sci_setparam(int a1, int a2, int a3, char a4) {
    return 1;
}

void extdev_attach() {

    // get instance
    for (auto &name : EXTDEV_INSTANCE_NAMES) {
        auto instance = libutils::try_module(name);
        if (instance) {
            EXTDEV_INSTANCE = instance;
        } else {
            continue;
        }

        // card unit
        detour::inline_hook((void *) cardunit_boot2, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_boot2", "?cardunit_boot2@@YAXHHH@Z"}));
        detour::inline_hook((void *) cardunit_boot, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_boot", "?cardunit_boot@@YAXHH@Z"}));
        detour::inline_hook((void *) cardunit_boot_initialize, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_boot_initialize", "?cardunit_boot_initialize@@YAHXZ"}));
        detour::inline_hook((void *) cardunit_boot_no_slot_type, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_boot_no_slot_type", "?cardunit_boot_no_slot_type@@YAXHH@Z"}));
        detour::inline_hook((void *) cardunit_card_eject, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_card_eject", "?cardunit_card_eject@@YAXH@Z"}));
        detour::inline_hook((void *) cardunit_card_eject_complete, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_card_eject_complete", "?cardunit_card_eject_complete@@YAHH@Z"}));
        detour::inline_hook((void *) cardunit_card_eject_wait, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_card_eject_wait", "?cardunit_card_eject_wait@@YAHH@Z"}));
        detour::inline_hook((void *) cardunit_card_read2, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_card_read2",
                                  "?cardunit_card_read2@@YAHHQAEPAH@Z",
                                  "?cardunit_card_read2@@YAHHQEAEPEAH@Z"}));
        detour::inline_hook((void *) cardunit_card_read, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_card_read", "?cardunit_card_read@@YAHHQAE@Z"}));
        detour::inline_hook((void *) cardunit_card_ready, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_card_ready", "?cardunit_card_ready@@YAXH@Z"}));
        detour::inline_hook((void *) cardunit_card_sensor, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_card_sensor", "?cardunit_card_sensor@@YAHH@Z"}));
        detour::inline_hook((void *) cardunit_card_sensor_raw, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_card_sensor_raw", "?cardunit_card_sensor_raw@@YAHH@Z"}));
        detour::inline_hook((void *) cardunit_update, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_update", "?cardunit_update@@YAXXZ"}));
        detour::inline_hook((void *) cardunit_get_status, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_get_status", "?cardunit_get_status@@YAHH@Z"}));
        detour::inline_hook((void *) cardunit_cardnumber_obfuscate_decode, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_cardnumber_obfuscate_decode",
                                  "?cardunit_cardnumber_obfuscate_decode@@YAHPEADHPEBD1@Z"}));
        detour::inline_hook((void *) cardunit_cardnumber_obfuscate_encode, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_cardnumber_obfuscate_encode",
                                  "?cardunit_cardnumber_obfuscate_encode@@YAHPEADHPEBD1@Z"}));
        detour::inline_hook((void *) cardunit_get_errorcount, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_get_errorcount", "?cardunit_get_errorcount@@YAHH@Z"}));
        detour::inline_hook((void *) cardunit_get_recvcount, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_get_recvcount", "?cardunit_get_recvcount@@YAHH@Z"}));
        detour::inline_hook((void *) cardunit_get_sendcount, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_get_sendcount", "?cardunit_get_sendcount@@YAHH@Z"}));
        detour::inline_hook((void *) cardunit_get_version, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_get_version",
                                  "?cardunit_get_version@@YAPBUcardunit_firm_version@@H@Z"}));
        detour::inline_hook((void *) cardunit_check_version, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_check_version", "?cardunit_check_version@@YAEH@Z"}));
        detour::inline_hook((void *) cardunit_key_get, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_key_get", "?cardunit_key_get@@YAHH@Z"}));
        detour::inline_hook((void *) cardunit_key_str, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_key_str", "?cardunit_key_str@@YAPBDH@Z"}));
        detour::inline_hook((void *) cardunit_reset, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_reset", "?cardunit_reset@@YAHXZ"}));
        detour::inline_hook((void *) cardunit_shutdown, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_shutdown", "?cardunit_shutdown@@YAXXZ"}));
        detour::inline_hook((void *) cardunit_sleep, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"cardunit_sleep", "?cardunit_sleep@@YAXH@Z"}));

        // GFDM UNIT
        detour::inline_hook((void *) gfdm_unit_boot, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_boot",
                                  "?gfdm_unit_boot@@YAXW4UNIT_TYPE@@HH@Z"}));
        detour::inline_hook((void *) gfdm_unit_boot_initialize, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_boot_initialize",
                                  "?gfdm_unit_boot_initialize@@YAHXZ"}));
        detour::inline_hook((void *) gfdm_unit_get_button, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_get_button",
                                  "?gfdm_unit_get_button@@YA?AUunit_button_t@@H@Z"}));
        detour::inline_hook((void *) gfdm_unit_get_button_dm, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_get_button_dm",
                                  "?gfdm_unit_get_button_dm@@YAHW4DM_BUTTON_DEFINITION@@@Z"}));
        detour::inline_hook((void *) gfdm_unit_get_button_gf, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_get_button_gf",
                                  "?gfdm_unit_get_button_gf@@YAHW4UNIT_NO@@W4GF_BUTTON_DEFINITION@@@Z"}));
        detour::inline_hook((void *) gfdm_unit_get_dm_attack_border_value, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_get_dm_attack_border_value",
                                  "?gfdm_unit_get_dm_attack_border_value@@YAHHPEAH@Z"}));
        detour::inline_hook((void *) gfdm_unit_get_errorcount, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_get_errorcount",
                                  "?gfdm_unit_get_errorcount@@YAHH@Z"}));
        detour::inline_hook((void *) gfdm_unit_get_input, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_get_input",
                                  "?gfdm_unit_get_input@@YAIH@Z"}));
        detour::inline_hook((void *) gfdm_unit_get_sensor_gf, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_get_sensor_gf",
                                  "?gfdm_unit_get_sensor_gf@@YAHW4UNIT_NO@@W4GF_SENSOR_DEFINITION@@@Z"}));
        detour::inline_hook((void *) gfdm_unit_get_status, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_get_status",
                                  "?gfdm_unit_get_status@@YAHH@Z"}));
        detour::inline_hook((void *) gfdm_unit_get_stream_errorcount, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_get_stream_errorcount",
                                  "?gfdm_unit_get_stream_errorcount@@YAHH@Z"}));
        detour::inline_hook((void *) gfdm_unit_get_version, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_get_version",
                                  "?gfdm_unit_get_version@@YAPEBUfirm_version@@H@Z"}));
        detour::inline_hook((void *) gfdm_unit_reset, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_reset",
                                  "?gfdm_unit_reset@@YAHXZ"}));
        detour::inline_hook((void *) gfdm_unit_reset_stream_errorcount, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_reset_stream_errorcount",
                                  "?gfdm_unit_reset_stream_errorcount@@YAXH@Z"}));
        detour::inline_hook((void *) gfdm_unit_send_motor_value, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_send_motor_value",
                                  "?gfdm_unit_send_motor_value@@YAIHE@Z"}));
        detour::inline_hook((void *) gfdm_unit_set_dm_attack_border_value, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_set_dm_attack_border_value",
                                  "?gfdm_unit_set_dm_attack_border_value@@YAXQEAH@Z"}));
        detour::inline_hook((void *) gfdm_unit_shutdown, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_shutdown",
                                  "?gfdm_unit_shutdown@@YAXXZ"}));
        detour::inline_hook((void *) gfdm_unit_update, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit_update",
                                  "?gfdm_unit_update@@YAXXZ"}));

        // GFDM UNIT 2
        detour::inline_hook((void *) gfdm_unit2_boot, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_boot",
                                  "?gfdm_unit2_boot@@YAXW4UNIT_TYPE@@HH@Z"}));
        detour::inline_hook((void *) gfdm_unit2_boot_initialize, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_boot_initialize",
                                  "?gfdm_unit2_boot_initialize@@YAHXZ"}));
        detour::inline_hook((void *) gfdm_unit2_get_button, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_get_button",
                                  "?gfdm_unit2_get_button@@YA?AUunit_button_t@@H@Z"}));
        detour::inline_hook((void *) gfdm_unit2_get_button_dm, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_get_button_dm",
                                  "?gfdm_unit2_get_button_dm@@YAHW4DM_BUTTON_DEFINITION@@@Z"}));
        detour::inline_hook((void *) gfdm_unit2_get_button_gf, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_get_button_gf",
                                  "?gfdm_unit2_get_button_gf@@YAHW4UNIT_NO@@W4GF_BUTTON_DEFINITION@@@Z"}));
        detour::inline_hook((void *) gfdm_unit2_get_dm_attack_border_value, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_get_dm_attack_border_value",
                                  "?gfdm_unit2_get_dm_attack_border_value@@YAHHPEAH@Z"}));
        detour::inline_hook((void *) gfdm_unit2_get_errorcount, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_get_errorcount",
                                  "?gfdm_unit2_get_errorcount@@YAHH@Z"}));
        detour::inline_hook((void *) gfdm_unit2_get_input, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_get_input",
                                  "?gfdm_unit2_get_input@@YAIH@Z"}));
        detour::inline_hook((void *) gfdm_unit2_get_sensor_gf, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_get_sensor_gf",
                                  "?gfdm_unit2_get_sensor_gf@@YAHW4UNIT_NO@@W4GF_SENSOR_DEFINITION@@@Z"}));
        detour::inline_hook((void *) gfdm_unit2_get_status, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_get_status",
                                  "?gfdm_unit2_get_status@@YAHH@Z"}));
        detour::inline_hook((void *) gfdm_unit2_get_stream_errorcount, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_get_stream_errorcount",
                                  "?gfdm_unit2_get_stream_errorcount@@YAHH@Z"}));
        detour::inline_hook((void *) gfdm_unit2_get_version, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_get_version",
                                  "?gfdm_unit2_get_version@@YAPEBUfirm_version@@H@Z"}));
        detour::inline_hook((void *) gfdm_unit2_reset, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_reset",
                                  "?gfdm_unit2_reset@@YAHXZ"}));
        detour::inline_hook((void *) gfdm_unit2_reset_stream_errorcount, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_reset_stream_errorcount",
                                  "?gfdm_unit2_reset_stream_errorcount@@YAXH@Z"}));
        detour::inline_hook((void *) gfdm_unit2_send_motor_value, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_send_motor_value",
                                  "?gfdm_unit2_send_motor_value@@YAIHE@Z"}));
        detour::inline_hook((void *) gfdm_unit2_set_dm_attack_border_value, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_set_dm_attack_border_value",
                                  "?gfdm_unit2_set_dm_attack_border_value@@YAXQEAH@Z"}));
        detour::inline_hook((void *) gfdm_unit2_shutdown, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_shutdown",
                                  "?gfdm_unit2_shutdown@@YAXXZ"}));
        detour::inline_hook((void *) gfdm_unit2_update, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"gfdm_unit2_update",
                                  "?gfdm_unit2_update@@YAXXZ"}));

        // led unit
        detour::inline_hook((void *) led_get_version, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_get_version",
                                  "?led_get_version@@YAPEBUfirm_version@@H@Z"}));
        detour::inline_hook((void *) led_unit_boot, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_unit_boot",
                                  "?led_unit_boot@@YAXHH@Z"}));
        detour::inline_hook((void *) led_unit_boot_initialize, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_unit_boot_initialize",
                                  "?led_unit_boot_initialize@@YAHXZ"}));
        detour::inline_hook((void *) led_unit_get_errorcount, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_unit_get_errorcount",
                                  "?led_unit_get_errorcount@@YAHH@Z"}));
        detour::inline_hook((void *) led_unit_get_status, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_unit_get_status",
                                  "?led_unit_get_status@@YAHH@Z"}));
        detour::inline_hook((void *) led_unit_reset, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_unit_reset",
                                  "?led_unit_reset@@YAHXZ"}));
        detour::inline_hook((void *) led_unit_send_custom1, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_unit_send_custom1",
                                  "?led_unit_send_custom1@@YAIHQEAE@Z"}));
        detour::inline_hook((void *) led_unit_send_custom2, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_unit_send_custom2",
                                  "?led_unit_send_custom2@@YAIHQEAE@Z"}));
        detour::inline_hook((void *) led_unit_send_direct, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_unit_send_direct",
                                  "?led_unit_send_direct@@YAIHQEAE@Z"}));
        detour::inline_hook((void *) led_unit_shutdown, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_unit_shutdown",
                                  "?led_unit_shutdown@@YAXXZ"}));
        detour::inline_hook((void *) led_unit_update, libutils::try_proc_list(
                EXTDEV_INSTANCE, {"led_unit_update",
                                  "?led_unit_update@@YAXXZ"}));

        // SCI
        detour::inline_hook((void *) sci_boot, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_boot"));
        detour::inline_hook((void *) sci_clear_error, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_clear_error"));
        detour::inline_hook((void *) sci_close, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_close"));
        detour::inline_hook((void *) sci_flush, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_flush"));
        detour::inline_hook((void *) sci_flush_complete, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_flush_complete"));
        detour::inline_hook((void *) sci_get_error, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_get_error"));
        detour::inline_hook((void *) sci_gets, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_gets"));
        detour::inline_hook((void *) sci_print_error, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_print_error"));
        detour::inline_hook((void *) sci_puts, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_puts"));
        detour::inline_hook((void *) sci_set_linebreak, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_set_linebreak"));
        detour::inline_hook((void *) sci_setparam, libutils::try_proc(
                EXTDEV_INSTANCE, "sci_setparam"));
    }
}

void extdev_detach() {
    // TODO
}
