#pragma once

#include "overlay/window.h"

namespace overlay::windows {

    class Keypad : public Window {
    private:

        size_t unit = 0;

    public:

        Keypad(SpiceOverlay *overlay, size_t unit);
        ~Keypad() override;

        void build_content() override;
    };
}
