#include "gitadora.h"

#include <unordered_map>

#include "hooks/graphics/graphics.h"
#include "util/detour.h"
#include "util/libutils.h"
#include "util/logging.h"
#include "util/sigscan.h"

namespace games::gitadora {

    // settings
    bool TWOCHANNEL = false;
    std::optional<unsigned int> CAB_TYPE = std::nullopt;

    /*
     * GitaDora checks if the IP address has changed, and if it has it throws 5-1506-0000 like jubeat.
     * We don't want this so we patch it out.
     */
    static char __cdecl eam_network_detected_ip_change() {
        return 0;
    }

    /*
     * GitaDora checks if the server it connects to is in the 192.168.0.0/16 or 169.254.0.0/16 subnet.
     * If it is, it downright refuses to use it and errors with no visible indication.
     * We don't want this so we patch it out.
     */
    static char __cdecl eam_network_settings_conflict() {
        return 0;
    }

    /*
     * Prevent GitaDora from changing the volume setting.
     */
    static long __cdecl bmsd2_set_windows_volume(int volume) {
        return 0;
    }

    /*
     * Two Channel Audio Mode
     * We proxy bmsd2_boot_hook and modify the last parameter which is apparently the channel count.
     * Since this apparently isn't the only thing required we need a signature scan to modify a value as well.
     */
    typedef int (__cdecl *bmsd2_boot_t)(long a1, int a2, long a3, char channel_count);
    static bmsd2_boot_t bmsd2_boot_orig = nullptr;
    static int __cdecl bmsd2_boot_hook(long a1, int a2, long a3, char channel_count) {
        return bmsd2_boot_orig(a1, a2, a3, 2);
    }

    /*
     * Command Line Arguments
     * We hook this to override specific values.
     * This currently disables the ability to specify your own in the app-config.xml (param/cmdline __type="str")
     */
    static bool __cdecl sys_code_get_cmdline(const char *cmdline) {
        if (strcmp(cmdline, "-d") == 0) {
            return true;
        } else if (strcmp(cmdline, "-DM") == 0) {
            return true;
        } else if (strcmp(cmdline, "-WINDOW") == 0) {
            return GRAPHICS_WINDOWED;
        } else if (strcmp(cmdline, "-LOGOUT") == 0) {
            return false;
        } else if (strcmp(cmdline, "-AOU") == 0) {
            return false;
        } else if (strcmp(cmdline, "-QCMODE") == 0) {
            return false;
        } else if (strcmp(cmdline, "-FACTORY") == 0) {
            return false;
        }
        return false;
    }

    /*
     * System Setting Parameter Overrides
     */
    static std::unordered_map<std::string, long> SYS_SETTINGS;
    static std::unordered_map<std::string, long> SYS_DEBUG_DIPS;

    static long __cdecl sys_setting_get_param(const char *param) {

        // overrides
        if (strcmp(param, "PRODUCTION_MODE") == 0) {
            return 0;
        } else if (strcmp(param, "ENABLE_DISP_ID") == 0) {
            return 0;
        } else if (CAB_TYPE.has_value() && strcmp(param, "VER_MACHINE") == 0) {
            return CAB_TYPE.value() << 12;
        }

        // map lookup
        auto it = SYS_SETTINGS.find(param);
        if (it != SYS_SETTINGS.end()) {
            return it->second;
        }

        return -1;
    }

    static long __cdecl sys_setting_set_param(const char *param, long value) {
        SYS_SETTINGS[std::string(param)] = value;

        return 1;
    }

    static long __cdecl sys_debug_dip_get_param(const char *param) {

        // overrides
        if (strcmp(param, "sysinfo") == 0) {
            return 0;
        } else if (strcmp(param, "jobbar1") == 0) {
            return 0;
        } else if (strcmp(param, "jobbar2") == 0) {
            return 0;
        } else if (strcmp(param, "serial") == 0) {
            return 0;
        } else if (strcmp(param, "warnvpf") == 0) {
            return 0;
        } else if (strcmp(param, "scrshot") == 0) {
            return 0;
        } else if (strcmp(param, "eamxml") == 0) {
            return 0;
        } else if (strcmp(param, "offset") == 0) {
            return 0;
        } else if (strcmp(param, "autodbg") == 0) {
            return 0;
        } else if (strcmp(param, "develop") == 0) {
            return 0;
        } else if (strcmp(param, "effect_test") == 0) {
            return 0;
        } else if (strcmp(param, "voice_type2") == 0) {
            return 0;
        }

        // map lookup
        auto it = SYS_DEBUG_DIPS.find(param);
        if (it != SYS_DEBUG_DIPS.end()) {
            return it->second;
        }

        return -1;
    }

    static long __cdecl sys_debug_dip_set_param(const char *param, long value) {
        SYS_DEBUG_DIPS[std::string(param)] = value;

        return 1;
    }

    GitaDoraGame::GitaDoraGame() : Game("GitaDora") {
    }

    void GitaDoraGame::attach() {
        Game::attach();

        // modules
        HMODULE sharepj_module = libutils::try_module("libshare-pj.dll");
        HMODULE bmsd_engine_module = libutils::try_module("libbmsd-engine.dll");
        HMODULE bmsd_module = libutils::try_module("libbmsd.dll");
        HMODULE bmsd2_module = libutils::try_module("libbmsd2.dll");
        HMODULE gdme_module = libutils::try_module("libgdme.dll");
        HMODULE system_module = libutils::try_module("libsystem.dll");

        // patches
        detour::inline_hook((void *) eam_network_detected_ip_change, libutils::try_proc(
                sharepj_module, "eam_network_detected_ip_change"));
        detour::inline_hook((void *) eam_network_settings_conflict, libutils::try_proc(
                sharepj_module, "eam_network_settings_conflict"));
        detour::inline_hook((void *) bmsd2_set_windows_volume, libutils::try_proc(
                bmsd2_module, "bmsd2_set_windows_volume"));
        detour::inline_hook((void *) bmsd2_set_windows_volume, libutils::try_proc(
                bmsd2_module, "bmsd2_set_windows_volume"));
        detour::inline_hook((void *) sys_code_get_cmdline, libutils::try_proc(
                system_module, "sys_code_get_cmdline"));
        detour::inline_hook((void *) sys_setting_get_param, libutils::try_proc(
                system_module, "sys_setting_get_param"));
        detour::inline_hook((void *) sys_setting_set_param, libutils::try_proc(
                system_module, "sys_setting_set_param"));
        detour::inline_hook((void *) sys_debug_dip_get_param, libutils::try_proc(
                system_module, "sys_debug_dip_get_param"));
        detour::inline_hook((void *) sys_debug_dip_set_param, libutils::try_proc(
                system_module, "sys_debug_dip_set_param"));

        // window patch
        if (GRAPHICS_WINDOWED && !replace_pattern(
                gdme_module,
                "754185ED753D8B4118BF0000CB02",
                "9090????9090??????????????12", 0, 0))
        {
            log_warning("gitadora", "windowed mode failed");
        }

        // two channel mod
        if (TWOCHANNEL) {
            bmsd2_boot_orig = detour::iat_try("bmsd2_boot", bmsd2_boot_hook, bmsd_module);

            if (!(replace_pattern(bmsd_engine_module, "33000000488D", "03??????????", 0, 0) ||
                    replace_pattern(bmsd_engine_module, "330000000F10", "03??????????", 0, 0)))
            {
                log_warning("gitadora", "two channel mode failed");
            }
        }
    }
}
