#include "superexit.h"

#include <thread>

#include "windows.h"
#include "launcher/shutdown.h"
#include "rawinput/rawinput.h"
#include "util/logging.h"
#include "touch/touch.h"


namespace superexit {

    static std::thread *THREAD = nullptr;
    static bool THREAD_RUNNING = false;

    bool has_focus() {
        HWND fg_wnd = GetForegroundWindow();
        if (fg_wnd == NULL) {
            return false;
        }
        if (fg_wnd == SPICETOUCH_TOUCH_HWND) {
            return true;
        }
        DWORD fg_pid;
        GetWindowThreadProcessId(fg_wnd, &fg_pid);
        return fg_pid == GetCurrentProcessId();
    }

    void enable() {

        // check if already running
        if (THREAD)
            return;

        // create new thread
        THREAD_RUNNING = true;
        THREAD = new std::thread([] {

            // log
            log_info("superexit", "enabled");

            // set variable to false to stop
            while (THREAD_RUNNING) {

                // check rawinput for ALT+F4
                bool rawinput_exit = false;
                if (RI_MGR != nullptr) {
                    auto devices = RI_MGR->devices_get();
                    for (auto &device : devices) {
                        switch (device.type) {
                            case rawinput::KEYBOARD: {
                                auto &key_states = device.keyboardInfo->key_states;
                                for (int page_index = 0; page_index < 1024; page_index += 256) {
                                    if (key_states[page_index + VK_MENU]
                                        && key_states[page_index + VK_F4]) {
                                        rawinput_exit = true;
                                    }
                                }
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }

                // check for exit
                if (rawinput_exit || (GetAsyncKeyState(VK_MENU) && GetAsyncKeyState(VK_F4))) {

                    // check if in focus
                    if (has_focus()) {
                        log_info("superexit", "detected ALT+F4, exiting...");
                        launcher::shutdown();
                    }
                }

                // slow down
                Sleep(100);
            }

            return nullptr;
        });
    }

    void disable() {

        // stop old thread
        THREAD_RUNNING = false;
        THREAD->join();

        // delete thread
        delete THREAD;
        THREAD = nullptr;

        // log
        log_info("superexit", "disabled");
    }
}
