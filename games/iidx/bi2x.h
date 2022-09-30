#pragma once

#include "acioemu/acioemu.h"
#include "hooks/devicehook.h"

namespace games::iidx {

    class BI2XSerialHandle : public CustomHandle {
    private:
        acioemu::ACIOEmu acio_emu;

        class BI2XDevice : public acioemu::ACIODeviceEmu {
        private:
            uint8_t coin_counter = 0;

        public:
            BI2XDevice();

            bool parse_msg(acioemu::MessageData *msg_in, circular_buffer<uint8_t> *response_buffer) override;
        };

    public:
        bool open(LPCWSTR lpFileName) override;
        int read(LPVOID lpBuffer, DWORD nNumberOfBytesToRead) override;
        int write(LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite) override;
        int device_io(DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize, LPVOID lpOutBuffer,
                      DWORD nOutBufferSize) override;
        bool close() override;
    };
}
