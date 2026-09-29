/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Minimal ST25R3916 NFC-A reader, ported from M5Stack's UiFlow2 driver (MIT):
 * https://github.com/m5stack/uiflow-micropython/blob/master/m5stack/libs/driver/st25r3916.py
 * Register values and timings follow that driver, which works on StackChan.
 */
#include "st25r3916.h"
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>
#include <iterator>

namespace {

constexpr int kI2cTimeoutMs = 50;

// I2C framing
constexpr uint8_t OP_READ      = 0x40;
constexpr uint8_t OP_LOAD_FIFO = 0x80;
constexpr uint8_t OP_READ_FIFO = 0x9F;
constexpr uint8_t CMD_SPACE_B  = 0xFB;

// Direct commands
constexpr uint8_t CMD_SET_DEFAULT          = 0xC1;
constexpr uint8_t CMD_STOP_ALL_ACTIVITIES  = 0xC2;
constexpr uint8_t CMD_TRANSMIT_WITH_CRC    = 0xC4;
constexpr uint8_t CMD_TRANSMIT_WITHOUT_CRC = 0xC5;
constexpr uint8_t CMD_TRANSMIT_REQA        = 0xC6;
constexpr uint8_t CMD_TRANSMIT_WUPA        = 0xC7;
constexpr uint8_t CMD_NFC_INITIAL_FIELD_ON = 0xC8;
constexpr uint8_t CMD_RESET_RX_GAIN        = 0xD5;
constexpr uint8_t CMD_ADJUST_REGULATORS    = 0xD6;
constexpr uint8_t CMD_CLEAR_FIFO           = 0xDB;
constexpr uint8_t CMD_TEST_ACCESS          = 0xFC;

// Registers (space A)
constexpr uint8_t REG_IO_CONFIGURATION_1       = 0x00;
constexpr uint8_t REG_IO_CONFIGURATION_2       = 0x01;
constexpr uint8_t REG_OPERATION_CONTROL        = 0x02;
constexpr uint8_t REG_MODE_DEFINITION          = 0x03;
constexpr uint8_t REG_BITRATE_DEFINITION       = 0x04;
constexpr uint8_t REG_ISO14443A_SETTINGS       = 0x05;
constexpr uint8_t REG_NFCIP1_PASSIVE_TARGET    = 0x08;
constexpr uint8_t REG_AUXILIARY_DEFINITION     = 0x0A;
constexpr uint8_t REG_RECEIVER_CONFIGURATION_1 = 0x0B;
constexpr uint8_t REG_NO_RESPONSE_TIMER_1      = 0x10;
constexpr uint8_t REG_TIMER_AND_EMV_CONTROL    = 0x12;
constexpr uint8_t REG_MASK_MAIN_INTERRUPT      = 0x16;
constexpr uint8_t REG_MAIN_INTERRUPT           = 0x1A;
constexpr uint8_t REG_FIFO_STATUS_1            = 0x1E;
constexpr uint8_t REG_NUMBER_OF_TX_BYTES_1     = 0x22;
constexpr uint8_t REG_ANTENNA_TUNING_1         = 0x26;
constexpr uint8_t REG_ANTENNA_TUNING_2         = 0x27;
constexpr uint8_t REG_TX_DRIVER                = 0x28;
constexpr uint8_t REG_PASSIVE_TARGET_MOD       = 0x29;
constexpr uint8_t REG_EXT_FIELD_ACT_THRESHOLD  = 0x2A;
constexpr uint8_t REG_EXT_FIELD_DEACT_THRESH   = 0x2B;
constexpr uint8_t REG_AUXILIARY_DISPLAY        = 0x31;
constexpr uint8_t REG_IC_IDENTITY              = 0x3F;

// Registers (space B)
constexpr uint8_t REG_B_EMD_SUPPRESSION = 0x05;
constexpr uint8_t REG_B_CORRELATOR_1    = 0x0C;
constexpr uint8_t REG_B_CORRELATOR_2    = 0x0D;
constexpr uint8_t REG_B_RESISTIVE_AM    = 0x2A;
constexpr uint8_t REG_B_OVERSHOOT_1     = 0x30;
constexpr uint8_t REG_B_OVERSHOOT_2     = 0x31;
constexpr uint8_t REG_B_UNDERSHOOT_1    = 0x32;
constexpr uint8_t REG_B_UNDERSHOOT_2    = 0x33;

// Bits and values
constexpr uint8_t OP_EN     = 0x80;
constexpr uint8_t WU        = 0x04;
constexpr uint8_t I_OSC     = 0x80;
constexpr uint8_t OSC_OK    = 0x10;
constexpr uint8_t AAT_EN    = 0x20;
constexpr uint8_t TX_EN     = 0x08;
constexpr uint8_t RX_EN     = 0x40;
constexpr uint8_t NO_CRC_RX = 0x80;
constexpr uint8_t ANTCL     = 0x01;
constexpr uint8_t DIS_CORR  = 0x04;

constexpr uint32_t TIMEOUT_REQ_WUP_MS  = 12;
constexpr uint32_t TIMEOUT_ANTICOLL_MS = 8;
constexpr uint32_t TIMEOUT_SELECT_MS   = 8;
constexpr uint32_t TIMEOUT_T2_READ_MS  = 24;
constexpr uint8_t CASCADE_TAG          = 0x88;
constexpr uint8_t SAK_CASCADE_FLAG     = 0x04;
constexpr uint64_t FC_HZ               = 13560000;

// Short waits busy-wait: a FreeRTOS tick is 10 ms here.
void sleepMs(uint32_t ms)
{
    if (ms < portTICK_PERIOD_MS) {
        esp_rom_delay_us(ms * 1000);
    } else {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

uint32_t nowMs()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// NFC Forum URI record prefixes (URI RTD 1.0, codes 0x00..0x23)
const char* const kUriPrefixes[] = {
    "", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:", "ftp://anonymous:anonymous@",
    "ftp://ftp.", "ftps://", "sftp://", "smb://", "nfs://", "ftp://", "dav://", "news:", "telnet://", "imap:",
    "rtsp://", "urn:", "pop:", "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://", "tcpobex://",
    "irdaobex://", "file://", "urn:epc:id:", "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:", "urn:nfc:",
};

}  // namespace

ST25R3916::~ST25R3916()
{
    end();
}

bool ST25R3916::begin(i2c_master_bus_handle_t bus, uint8_t address, uint32_t sclHz)
{
    end();
    if (!bus || i2c_master_probe(bus, address, kI2cTimeoutMs) != ESP_OK) {
        return false;
    }
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length     = I2C_ADDR_BIT_LEN_7;
    cfg.device_address      = address;
    cfg.scl_speed_hz        = sclHz;
    if (i2c_master_bus_add_device(bus, &cfg, &_dev) != ESP_OK) {
        _dev = nullptr;
        return false;
    }
    if (!initChip() || !configureNfcA()) {
        end();
        return false;
    }
    fieldOff();
    return true;
}

void ST25R3916::end()
{
    if (_dev) {
        fieldOff();
        i2c_master_bus_rm_device(_dev);
        _dev = nullptr;
    }
}

/* ---------------------------------- I/O ---------------------------------- */

bool ST25R3916::cmd(uint8_t command, const uint8_t* payload, size_t len)
{
    uint8_t buf[8];
    buf[0] = command;
    len    = std::min(len, sizeof(buf) - 1);
    if (payload && len) {
        std::memcpy(buf + 1, payload, len);
    }
    bool ok = i2c_master_transmit(_dev, buf, 1 + len, kI2cTimeoutMs) == ESP_OK;
    sleepMs(2);  // the bus must not be used while a direct command runs
    return ok;
}

uint8_t ST25R3916::rd(uint8_t reg)
{
    uint8_t addr = reg | OP_READ, value = 0;
    i2c_master_transmit_receive(_dev, &addr, 1, &value, 1, kI2cTimeoutMs);
    return value;
}

bool ST25R3916::wr(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(_dev, buf, 2, kI2cTimeoutMs) == ESP_OK;
}

void ST25R3916::wr16(uint8_t reg, uint16_t value)
{
    wr(reg, value >> 8);
    wr(reg + 1, value & 0xFF);
}

void ST25R3916::wr32(uint8_t reg, uint32_t value)
{
    wr(reg, value >> 24);
    wr(reg + 1, (value >> 16) & 0xFF);
    wr(reg + 2, (value >> 8) & 0xFF);
    wr(reg + 3, value & 0xFF);
}

void ST25R3916::wrSpaceB(uint8_t reg, uint8_t value)
{
    uint8_t buf[3] = {CMD_SPACE_B, (uint8_t)(reg & 0x3F), value};
    i2c_master_transmit(_dev, buf, 3, kI2cTimeoutMs);
    esp_rom_delay_us(50);
}

void ST25R3916::setBits(uint8_t reg, uint8_t bits)
{
    wr(reg, rd(reg) | bits);
}

void ST25R3916::clearBits(uint8_t reg, uint8_t bits)
{
    wr(reg, rd(reg) & ~bits);
}

void ST25R3916::clearInterrupts()
{
    for (uint8_t i = 0; i < 4; i++) {
        rd(REG_MAIN_INTERRUPT + i);  // reading clears
    }
}

void ST25R3916::writeFifo(const uint8_t* data, size_t len)
{
    uint8_t buf[24];
    buf[0] = OP_LOAD_FIFO;
    len    = std::min(len, sizeof(buf) - 1);
    std::memcpy(buf + 1, data, len);
    i2c_master_transmit(_dev, buf, 1 + len, kI2cTimeoutMs);
    esp_rom_delay_us(50);
}

size_t ST25R3916::readFifo(uint8_t* out, size_t len)
{
    uint8_t op = OP_READ_FIFO;
    return i2c_master_transmit_receive(_dev, &op, 1, out, len, kI2cTimeoutMs) == ESP_OK ? len : 0;
}

size_t ST25R3916::fifoBytes()
{
    uint16_t s = (uint16_t)(rd(REG_FIFO_STATUS_1) << 8) | rd(REG_FIFO_STATUS_1 + 1);
    return (s >> 8) | ((s & 0x00C0) << 2);
}

// Waits until the FIFO holds at least `need` bytes; with `stable`, also until it
// stops growing (end of the frame).
size_t ST25R3916::waitFifo(size_t need, uint32_t timeoutMs, bool stable)
{
    const uint32_t t0 = nowMs();
    while (nowMs() - t0 < timeoutMs) {
        size_t n = fifoBytes();
        if (n >= need) {
            if (!stable) {
                return n;
            }
            sleepMs(1);
            if (fifoBytes() == n) {
                return n;
            }
        }
        sleepMs(1);
    }
    return 0;
}

void ST25R3916::setNoResponseTimer(uint32_t ms)
{
    const bool step4096 = rd(REG_TIMER_AND_EMV_CONTROL) & 0x01;
    const uint64_t step = step4096 ? 4096ULL * 1000000 : 64ULL * 1000000;
    uint64_t nrt        = ((uint64_t)ms * 1000 * FC_HZ + step - 1) / step;
    wr16(REG_NO_RESPONSE_TIMER_1, (uint16_t)std::clamp<uint64_t>(nrt, 1, 0xFFFF));
}

void ST25R3916::setTxBytes(size_t bytes)
{
    wr16(REG_NUMBER_OF_TX_BYTES_1, (uint16_t)((bytes & 0x01FF) << 3));
}

/* ------------------------------- Chip setup ------------------------------- */

bool ST25R3916::initChip()
{
    const uint8_t id = rd(REG_IC_IDENTITY);
    if (id == 0xFF || ((id >> 3) & 0x1F) != 0x05 || (id & 0x07) == 0) {
        return false;  // not an ST25R3916
    }
    cmd(CMD_SET_DEFAULT);
    sleepMs(10);
    const uint8_t test[] = {0x04, 0x10};
    cmd(CMD_TEST_ACCESS, test, sizeof(test));

    wr16(REG_IO_CONFIGURATION_1, 0x1084);  // I2C, 3.3 V
    wr(REG_TX_DRIVER, 13 << 4);            // default AM modulation depth
    wr(REG_IO_CONFIGURATION_1, (rd(REG_IO_CONFIGURATION_1) & ~0x07) | 0x07);

    wrSpaceB(REG_B_RESISTIVE_AM, 0x80);
    setBits(REG_IO_CONFIGURATION_2, AAT_EN);
    wrSpaceB(REG_B_RESISTIVE_AM, 0x00);

    wr(REG_EXT_FIELD_ACT_THRESHOLD, 0x10 | 0x03);
    wr(REG_EXT_FIELD_DEACT_THRESH, 0x00 | 0x02);
    wr(REG_NFCIP1_PASSIVE_TARGET, (rd(REG_NFCIP1_PASSIVE_TARGET) & ~0xF0) | (0x05 << 4));
    wr(REG_PASSIVE_TARGET_MOD, 0x5F);
    wrSpaceB(REG_B_EMD_SUPPRESSION, 0x40);
    wr(REG_ANTENNA_TUNING_1, 0x82);
    wr(REG_ANTENNA_TUNING_2, 0x82);

    setBits(REG_OPERATION_CONTROL, 0x03);
    cmd(CMD_CLEAR_FIFO);
    wr32(REG_MASK_MAIN_INTERRUPT, 0xFFFF00FF);
    clearInterrupts();

    // Oscillator on
    if (!(rd(REG_OPERATION_CONTROL) & OP_EN)) {
        wr(REG_MASK_MAIN_INTERRUPT, rd(REG_MASK_MAIN_INTERRUPT) & ~I_OSC);
        clearInterrupts();
        setBits(REG_OPERATION_CONTROL, OP_EN);
        bool osc        = false;
        const uint32_t t0 = nowMs();
        while (nowMs() - t0 < 25) {
            sleepMs(1);
            if (rd(REG_MAIN_INTERRUPT) & I_OSC) {
                osc = true;
                break;
            }
        }
        setBits(REG_MASK_MAIN_INTERRUPT, I_OSC);
        if (!osc) {
            return false;
        }
    }
    if (!(rd(REG_AUXILIARY_DISPLAY) & OSC_OK)) {
        return false;
    }

    wr32(REG_MASK_MAIN_INTERRUPT, 0);
    cmd(CMD_ADJUST_REGULATORS);
    sleepMs(5);
    return true;
}

bool ST25R3916::configureNfcA()
{
    cmd(CMD_STOP_ALL_ACTIVITIES);
    clearBits(REG_OPERATION_CONTROL, WU);
    wr(REG_MODE_DEFINITION, 0x09);     // ISO14443A initiator
    wr(REG_BITRATE_DEFINITION, 0x00);  // 106 kbps
    wr(REG_ISO14443A_SETTINGS, 0x00);
    clearBits(REG_AUXILIARY_DEFINITION, DIS_CORR);
    wrSpaceB(REG_B_OVERSHOOT_1, 0x40);
    wrSpaceB(REG_B_OVERSHOOT_2, 0x03);
    wrSpaceB(REG_B_UNDERSHOOT_1, 0x40);
    wrSpaceB(REG_B_UNDERSHOOT_2, 0x03);
    wrSpaceB(REG_B_CORRELATOR_1, 0x47);
    wrSpaceB(REG_B_CORRELATOR_2, 0x00);
    wr(REG_RECEIVER_CONFIGURATION_1, 0x08);
    wr(REG_RECEIVER_CONFIGURATION_1 + 1, 0x2D);
    wr(REG_RECEIVER_CONFIGURATION_1 + 2, 0xD8);
    wr(REG_RECEIVER_CONFIGURATION_1 + 3, 0x22);
    wr32(REG_MASK_MAIN_INTERRUPT, 0);
    return cmd(CMD_RESET_RX_GAIN);
}

void ST25R3916::fieldOn()
{
    cmd(CMD_NFC_INITIAL_FIELD_ON);
    sleepMs(5);  // ISO14443 guard time
    setBits(REG_OPERATION_CONTROL, TX_EN | RX_EN);
}

void ST25R3916::fieldOff()
{
    if (!_dev) {
        return;
    }
    cmd(CMD_STOP_ALL_ACTIVITIES);
    clearBits(REG_OPERATION_CONTROL, TX_EN | RX_EN);
}

/* --------------------------------- NFC-A --------------------------------- */

bool ST25R3916::requestWakeup(bool reqa, uint16_t& atqa)
{
    setNoResponseTimer(TIMEOUT_REQ_WUP_MS);
    wr(REG_ISO14443A_SETTINGS, ANTCL);
    setBits(REG_AUXILIARY_DEFINITION, NO_CRC_RX);
    clearInterrupts();
    cmd(CMD_CLEAR_FIFO);
    cmd(reqa ? CMD_TRANSMIT_REQA : CMD_TRANSMIT_WUPA);
    const size_t n = waitFifo(2, TIMEOUT_REQ_WUP_MS);
    uint8_t rb[8];
    if (n < 2 || !readFifo(rb, std::min(n, sizeof(rb)))) {
        return false;
    }
    atqa = (uint16_t)(rb[1] << 8) | rb[0];
    return atqa != 0 && atqa != 0xFFFF;
}

bool ST25R3916::antiCollision(uint8_t selCmd, uint8_t out[5])
{
    setNoResponseTimer(TIMEOUT_ANTICOLL_MS);
    wr(REG_ISO14443A_SETTINGS, ANTCL);
    clearBits(REG_AUXILIARY_DEFINITION, NO_CRC_RX);
    clearInterrupts();
    cmd(CMD_CLEAR_FIFO);
    const uint8_t frame[] = {selCmd, 0x20};
    writeFifo(frame, sizeof(frame));
    setTxBytes(sizeof(frame));
    cmd(CMD_TRANSMIT_WITHOUT_CRC);
    const size_t n = waitFifo(5, TIMEOUT_ANTICOLL_MS);
    uint8_t rb[8];
    if (n < 5 || !readFifo(rb, std::min(n, sizeof(rb)))) {
        return false;
    }
    if ((rb[0] ^ rb[1] ^ rb[2] ^ rb[3]) != rb[4]) {
        return false;  // BCC mismatch, e.g. two tags at once
    }
    std::memcpy(out, rb, 5);
    return true;
}

bool ST25R3916::select(uint8_t selCmd, const uint8_t uid4[4], uint8_t bcc, uint8_t& sak)
{
    const uint8_t frame[] = {selCmd, 0x70, uid4[0], uid4[1], uid4[2], uid4[3], bcc};
    setNoResponseTimer(TIMEOUT_SELECT_MS);
    wr(REG_ISO14443A_SETTINGS, 0x00);
    clearBits(REG_AUXILIARY_DEFINITION, NO_CRC_RX);
    clearInterrupts();
    cmd(CMD_CLEAR_FIFO);
    writeFifo(frame, sizeof(frame));
    setTxBytes(sizeof(frame));
    cmd(CMD_TRANSMIT_WITH_CRC);
    const size_t n = waitFifo(1, TIMEOUT_SELECT_MS);
    uint8_t rb[4];
    if (n < 1 || !readFifo(rb, std::min(n, sizeof(rb)))) {
        return false;
    }
    sak = rb[0];
    return true;
}

bool ST25R3916::readUid(Tag& tag)
{
    tag = Tag{};
    if (!_dev) {
        return false;
    }
    if (!requestWakeup(true, tag.atqa) && !requestWakeup(false, tag.atqa)) {
        return false;
    }
    uint8_t cl1[5], cl2[5];
    if (!antiCollision(0x93, cl1) || !select(0x93, cl1, cl1[4], tag.sak)) {
        return false;
    }
    if (cl1[0] != CASCADE_TAG) {
        std::memcpy(tag.uid, cl1, 4);
        tag.uidLen = 4;
        return true;
    }
    if (!(tag.sak & SAK_CASCADE_FLAG) || !antiCollision(0x95, cl2) || !select(0x95, cl2, cl2[4], tag.sak) ||
        cl2[0] == CASCADE_TAG) {
        return false;  // 10-byte UIDs (cascade level 3) are not supported
    }
    const uint8_t uid[7] = {cl1[1], cl1[2], cl1[3], cl2[0], cl2[1], cl2[2], cl2[3]};
    std::memcpy(tag.uid, uid, 7);
    tag.uidLen = 7;
    return true;
}

size_t ST25R3916::transceiveCrc(const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxMax, uint32_t timeoutMs,
                                size_t minRx)
{
    setNoResponseTimer(timeoutMs);
    wr(REG_ISO14443A_SETTINGS, 0x00);
    clearBits(REG_AUXILIARY_DEFINITION, NO_CRC_RX);
    clearInterrupts();
    cmd(CMD_CLEAR_FIFO);
    writeFifo(tx, txLen);
    setTxBytes(txLen);
    cmd(CMD_TRANSMIT_WITH_CRC);
    const size_t n = waitFifo(minRx, timeoutMs, true);
    if (n == 0) {
        return 0;
    }
    return readFifo(rx, std::min(n, rxMax));
}

bool ST25R3916::readPages(uint8_t firstPage, uint8_t out[16])
{
    const uint8_t tx[] = {0x30, firstPage};
    uint8_t rx[20];
    if (transceiveCrc(tx, sizeof(tx), rx, sizeof(rx), TIMEOUT_T2_READ_MS, 18) < 16) {
        return false;  // 16 data bytes + 2 CRC bytes expected
    }
    std::memcpy(out, rx, 16);
    return true;
}

void ST25R3916::halt()
{
    const uint8_t tx[] = {0x50, 0x00};
    setNoResponseTimer(10);
    wr(REG_ISO14443A_SETTINGS, 0x00);
    clearBits(REG_AUXILIARY_DEFINITION, NO_CRC_RX);
    clearInterrupts();
    cmd(CMD_CLEAR_FIFO);
    writeFifo(tx, sizeof(tx));
    setTxBytes(sizeof(tx));
    cmd(CMD_TRANSMIT_WITH_CRC);
}

/* ---------------------------------- NDEF ---------------------------------- */

std::string ST25R3916::readNdefText()
{
    uint8_t page[16];
    // Page 3 is the capability container (0xE1 = NDEF formatted); user data starts at page 4.
    if (!readPages(3, page) || page[0] != 0xE1) {
        return "";
    }
    std::string mem(reinterpret_cast<char*>(page) + 4, 12);
    for (uint8_t p = 7; mem.size() < 144; p += 4) {  // NTAG213 has 144 user bytes
        if (!readPages(p, page)) {
            break;  // past the end of the tag's memory
        }
        mem.append(reinterpret_cast<char*>(page), 16);
    }
    auto at = [&](size_t i) { return i < mem.size() ? (uint8_t)mem[i] : 0; };

    // TLV blocks: 0x00 NULL, 0x03 NDEF message, 0xFE terminator
    size_t i = 0;
    while (i < mem.size()) {
        const uint8_t type = at(i++);
        if (type == 0x00) {
            continue;
        }
        if (type == 0xFE) {
            return "";
        }
        size_t len = at(i++);
        if (len == 0xFF) {
            len = (at(i) << 8) | at(i + 1);
            i += 2;
        }
        if (type != 0x03) {
            i += len;
            continue;
        }
        // First NDEF record: header, type length, payload length (short or long), id length, type, payload
        size_t r            = i;
        const uint8_t hdr   = at(r++);
        const uint8_t tnf   = hdr & 0x07;
        const uint8_t tlen  = at(r++);
        uint32_t plen       = 0;
        if (hdr & 0x10) {
            plen = at(r++);
        } else {
            plen = (at(r) << 24) | (at(r + 1) << 16) | (at(r + 2) << 8) | at(r + 3);
            r += 4;
        }
        const uint8_t idLen = (hdr & 0x08) ? at(r++) : 0;
        const std::string rtype = mem.substr(std::min(r, mem.size()), tlen);
        r += tlen + idLen;
        if (r >= mem.size() || tnf != 0x01) {
            return "";  // only NFC Forum well-known records (URI, text)
        }
        const std::string payload = mem.substr(r, std::min<size_t>(plen, mem.size() - r));
        if (rtype == "U" && !payload.empty()) {
            const uint8_t code = (uint8_t)payload[0];
            const char* prefix = code < std::size(kUriPrefixes) ? kUriPrefixes[code] : "";
            return prefix + payload.substr(1);
        }
        if (rtype == "T" && !payload.empty()) {
            const size_t langLen = (uint8_t)payload[0] & 0x3F;
            return payload.size() > 1 + langLen ? payload.substr(1 + langLen) : "";
        }
        return "";
    }
    return "";
}
