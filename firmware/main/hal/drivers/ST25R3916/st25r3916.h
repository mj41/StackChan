/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 * SPDX-FileCopyrightText: M5Stack Technology CO LTD (the UiFlow2 MicroPython driver this is ported from)
 *
 * SPDX-License-Identifier: MIT
 *
 * Minimal ST25R3916 NFC-A (ISO14443A) reader over I2C, polling only.
 * Ported from M5Stack's UiFlow2 driver (MIT):
 * https://github.com/m5stack/uiflow-micropython/blob/master/m5stack/libs/driver/st25r3916.py
 * On StackChan the chip's IRQ pin is not connected, so everything is polled.
 */
#pragma once
#include <driver/i2c_master.h>
#include <cstddef>
#include <cstdint>
#include <string>

class ST25R3916 {
public:
    struct Tag {
        uint8_t uid[10] = {};
        uint8_t uidLen  = 0;  // 4 or 7
        uint16_t atqa   = 0;
        uint8_t sak     = 0;
    };

    ~ST25R3916();

    // Adds the device to the bus, checks its identity and configures NFC-A reader mode.
    bool begin(i2c_master_bus_handle_t bus, uint8_t address = 0x50, uint32_t sclHz = 100000);
    void end();

    // RF field. Keep it off between polls: the transmitter can draw a lot of current.
    void fieldOn();
    void fieldOff();

    // REQA/WUPA + anticollision + select (cascade levels 1 and 2). Needs the field on.
    bool readUid(Tag& tag);
    // NFC Forum Type 2 READ: 4 pages (16 bytes) from firstPage. Needs a selected tag.
    bool readPages(uint8_t firstPage, uint8_t out[16]);
    // First NDEF record of a Type 2 tag (NTAG) as text: a URI or the text of a "T"
    // record. Empty if there is none. Needs a selected tag.
    std::string readNdefText();
    // Type 2 tag memory from page 0 as hex, read 4 pages at a time until the tag
    // stops answering (its end) or maxBytes. Needs a selected tag.
    std::string readMemoryHex(size_t maxBytes = 1024);
    void halt();

private:
    i2c_master_dev_handle_t _dev = nullptr;

    bool cmd(uint8_t command, const uint8_t* payload = nullptr, size_t len = 0);
    uint8_t rd(uint8_t reg);
    bool wr(uint8_t reg, uint8_t value);
    void wr16(uint8_t reg, uint16_t value);
    void wr32(uint8_t reg, uint32_t value);
    void wrSpaceB(uint8_t reg, uint8_t value);
    void setBits(uint8_t reg, uint8_t bits);
    void clearBits(uint8_t reg, uint8_t bits);
    void clearInterrupts();
    void writeFifo(const uint8_t* data, size_t len);
    size_t readFifo(uint8_t* out, size_t len);
    size_t fifoBytes();
    size_t waitFifo(size_t need, uint32_t timeoutMs, bool stable = false);
    void setNoResponseTimer(uint32_t ms);
    void setTxBytes(size_t bytes);
    bool initChip();
    bool configureNfcA();
    bool requestWakeup(bool reqa, uint16_t& atqa);
    bool antiCollision(uint8_t selCmd, uint8_t out[5]);
    bool select(uint8_t selCmd, const uint8_t uid4[4], uint8_t bcc, uint8_t& sak);
    size_t transceiveCrc(const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxMax, uint32_t timeoutMs,
                         size_t minRx);
};
