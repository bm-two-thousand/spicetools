#include "game.h"

#include "launcher/launcher.h"
#include "util/fileutils.h"
#include "util/libutils.h"
#include "util/logging.h"

namespace avs {

    namespace game {

        // function names
        const char ENTRY_INIT_NAME[] = "dll_entry_init";
        const char ENTRY_MAIN_NAME[] = "dll_entry_main";

        // functions
        typedef bool (*ENTRY_INIT_T)(char *, void *);
        typedef void (*ENTRY_MAIN_T)(void);
        ENTRY_INIT_T dll_entry_init;
        ENTRY_MAIN_T dll_entry_main;

        // properties
        char MODEL[4] = {'0', '0', '0', '\x00'};
        char DEST[2] = {'0', '\x00'};
        char SPEC[2] = {'0', '\x00'};
        char REV[2] = {'0', '\x00'};
        char EXT[11] = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '\x00'};

        // handle
        HINSTANCE DLL_INSTANCE;
        std::string DLL_NAME;

        bool is_model(const char *model) {
            return _stricmp(MODEL, model) == 0;
        }

        bool is_model(const char *model, const char *ext) {
            return is_model(model) && is_ext(ext);
        }

        bool is_model(const std::initializer_list<const char *> model_list) {
            for (auto &model : model_list) {
                if (is_model(model)) {
                    return true;
                }
            }

            return false;
        }

        bool is_ext(const char *ext) {
            return _stricmp(EXT, ext) == 0;
        }

        bool is_ext(int datecode_min, int datecode_max) {

            // range check
            long datecode = strtol(EXT, NULL, 10);
            return datecode_min <= datecode && datecode <= datecode_max;
        }

        std::string get_identifier() {
            return fmt::format("{}:{}:{}:{}:{}",
                    avs::game::MODEL,
                    avs::game::DEST,
                    avs::game::SPEC,
                    avs::game::REV,
                    avs::game::EXT);
        }

        void load_dll() {
            log_info("avs-game", "loading DLL '{}'", DLL_NAME);

            // load game instance
            if (fileutils::verify_header_pe(MODULE_PATH / DLL_NAME)) {
                DLL_INSTANCE = libutils::load_library(MODULE_PATH / DLL_NAME);
            }

            // load entry points
            dll_entry_init = (ENTRY_INIT_T) libutils::get_proc(DLL_INSTANCE, ENTRY_INIT_NAME);
            dll_entry_main = (ENTRY_MAIN_T) libutils::get_proc(DLL_INSTANCE, ENTRY_MAIN_NAME);
            log_info("avs-game", "loaded successfully ({})", fmt::ptr(DLL_INSTANCE));
        }

        bool entry_init(char *sid_code, void *app_param) {
            auto current_entry_init = (ENTRY_INIT_T) libutils::get_proc(DLL_INSTANCE, ENTRY_INIT_NAME);

            if (dll_entry_init != current_entry_init) {
                log_info("avs-game", "dll_entry_init is hooked");

                dll_entry_init = current_entry_init;
            }

            return dll_entry_init(sid_code, app_param);
        }

        void entry_main() {
            auto current_entry_main = (ENTRY_MAIN_T) libutils::get_proc(DLL_INSTANCE, ENTRY_MAIN_NAME);

            if (dll_entry_main != current_entry_main) {
                log_info("avs-game", "dll_entry_main is hooked");

                dll_entry_main = current_entry_main;
            }

            dll_entry_main();
        }
    }
}
