/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace embody {

/**
 * @brief End-to-end encryption between this robot and the browsers its owner enrolled
 *        through the QR code, so a relay carries only ciphertext.
 *
 * Design and threat model: home-w42-eu docs/e2ee.md. Reference implementation and test
 * vectors: s-w42-eu-raw's e2e package (selfTest() checks this code against them).
 * mbedtls only: X25519, HKDF-SHA256 (from HMAC), HMAC-SHA256, AES-256-GCM (hardware AES).
 * The robot key and the enrolled browsers are kept in NVS ("embody_e2e").
 */
class E2E {
public:
    using Key = std::array<uint8_t, 32>;

    // Loads (or creates) the robot key and the enrolled browsers; starts a new epoch with a
    // new group key. False if the crypto failed.
    bool begin(const std::string& robotId);

    // A new pairing secret's QR fragment, "e2e=1.<R_pub>.<P>": what the robot appends to
    // the pairing URL it shows (the server never sees it). The previous secret stays valid.
    std::string fragment();

    // E2EEnroll {b, mac}: on success the browser is enrolled and its sealed group key is
    // returned as the E2EGroupKey body (JSON). The secret used is consumed.
    bool enroll(const std::string& browserPub, const std::string& mac, std::string& groupKeyBody);
    // E2EHello {b}: the current group key for an enrolled browser.
    bool hello(const std::string& browserId, std::string& groupKeyBody);
    // E2ECommand {b, n, c}: the opened command JSON {command, args, seq}; replays refused.
    bool openCommand(const std::string& browserId, const std::string& n, const std::string& c, std::string& plain);
    // A plaintext frame JSON {kind, body} as the E2EData body (JSON).
    bool sealData(const std::string& plainFrame, std::string& body);
    // type + payload as a 0x30 message.
    bool sealBinary(uint8_t type, const uint8_t* data, size_t len, std::string& out);
    // A browser's whole 0x31 message: the plaintext message it carries (type byte included).
    bool openBrowserBinary(const std::string& msg, std::string& plain);

    size_t enrolled() const;
    // Forgets every browser and starts a new epoch.
    void forgetAll();
    // Forgets one browser (its id, 16 hex); a new epoch, so it cannot read on. False: not enrolled.
    bool forget(const std::string& browserId);

    // Checks the implementation against the reference test vectors; logs and returns the result.
    static bool selfTest();

private:
    std::string _robot_id;
    Key _priv{}, _pub{};
    std::vector<Key> _browsers;              // enrolled browser keys
    std::map<std::string, Key> _pairwise;    // browser id -> K_B
    std::map<std::string, uint64_t> _seqs;   // browser id -> last command seq
    Key _group{};
    uint32_t _epoch = 0;
    std::vector<std::array<uint8_t, 16>> _secrets;  // current and previous pairing secret
    uint8_t _nonce_prefix[4]{};
    uint64_t _nonce_ctr = 0;
    mutable std::mutex _mu;

    void next_nonce(uint8_t out[12]);
    void add_browser(const Key& pub);
    void save();
    bool group_key_body(const std::string& browserId, std::string& body);
};

}  // namespace embody
