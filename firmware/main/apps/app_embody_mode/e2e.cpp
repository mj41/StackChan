/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "e2e.h"
#include <settings.h>
#include <mooncake_log.h>
#include <ArduinoJson.hpp>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <cstring>

using namespace embody;

static const char* _tag = "E2E";

static constexpr const char* _enroll_label  = "w42-e2e-enroll|";
static constexpr const char* _pairwise_info = "w42-e2e pairwise";
static constexpr const char* _group_label   = "w42-e2e group|";
static constexpr size_t _max_browsers       = 16;

/* --------------------------------- helpers -------------------------------- */

static int rng(void*, unsigned char* out, size_t len)
{
    esp_fill_random(out, len);
    return 0;
}

static std::string b64u(const uint8_t* data, size_t len)
{
    size_t olen = 0;
    std::string out(4 * ((len + 2) / 3) + 1, '\0');
    if (mbedtls_base64_encode((unsigned char*)out.data(), out.size(), &olen, data, len) != 0) {
        return "";
    }
    out.resize(olen);
    for (auto& c : out) {
        c = c == '+' ? '-' : c == '/' ? '_' : c;
    }
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    return out;
}

static bool unb64u(const std::string& in, std::string& out)
{
    std::string s = in;
    for (auto& c : s) {
        c = c == '-' ? '+' : c == '_' ? '/' : c;
    }
    while (s.size() % 4) {
        s.push_back('=');
    }
    size_t olen = 0;
    out.assign(s.size(), '\0');
    if (mbedtls_base64_decode((unsigned char*)out.data(), out.size(), &olen, (const unsigned char*)s.data(), s.size()) != 0) {
        return false;
    }
    out.resize(olen);
    return true;
}

static std::string hex(const uint8_t* data, size_t len)
{
    static const char* d = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < len; i++) {
        out.push_back(d[data[i] >> 4]);
        out.push_back(d[data[i] & 15]);
    }
    return out;
}

static void hmac(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[32])
{
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, klen, msg, mlen, out);
}

// HKDF-SHA256 for 32 bytes of output: PRK = HMAC(salt, ikm); OKM = HMAC(PRK, info | 0x01).
static void hkdf32(const uint8_t* ikm, size_t ikmLen, const uint8_t* salt, size_t saltLen, const std::string& info,
                   uint8_t out[32])
{
    uint8_t prk[32];
    hmac(salt, saltLen, ikm, ikmLen, prk);
    std::string t = info;
    t.push_back(1);
    hmac(prk, sizeof prk, (const uint8_t*)t.data(), t.size(), out);
}

// RFC 7748 X25519 public key of a 32-byte private key (mbedtls clamps it).
static bool x25519_public(const E2E::Key& priv, E2E::Key& pub)
{
    mbedtls_ecp_keypair kp;
    mbedtls_ecp_keypair_init(&kp);
    size_t olen = 0;
    bool ok = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_CURVE25519, &kp, priv.data(), priv.size()) == 0 &&
              mbedtls_ecp_keypair_calc_public(&kp, rng, nullptr) == 0 &&
              mbedtls_ecp_write_public_key(&kp, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub.data(), pub.size()) == 0 &&
              olen == pub.size();
    mbedtls_ecp_keypair_free(&kp);
    return ok;
}

// X25519(priv, peer): the shared secret; refuses the all-zero result (low-order points).
static bool x25519(const E2E::Key& priv, const E2E::Key& peer, E2E::Key& out)
{
    mbedtls_ecp_keypair kp;
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point q, own;
    mbedtls_ecp_keypair_init(&kp);
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&q);
    mbedtls_ecp_point_init(&own);
    bool ok = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_CURVE25519, &kp, priv.data(), priv.size()) == 0 &&
              mbedtls_ecp_export(&kp, &grp, &d, &own) == 0 &&
              mbedtls_ecp_point_read_binary(&grp, &q, peer.data(), peer.size()) == 0 &&
              mbedtls_ecdh_compute_shared(&grp, &z, &q, &d, rng, nullptr) == 0 &&
              mbedtls_mpi_write_binary_le(&z, out.data(), out.size()) == 0;
    mbedtls_ecp_point_free(&own);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    mbedtls_ecp_keypair_free(&kp);
    uint8_t any = 0;
    for (auto b : out) {
        any |= b;
    }
    return ok && any;
}

// K_B = HKDF-SHA256(X25519(own, peer), salt = R_pub | B_pub, info).
static bool pairwise(const E2E::Key& own, const E2E::Key& peer, const E2E::Key& r, const E2E::Key& b, E2E::Key& k)
{
    E2E::Key shared;
    if (!x25519(own, peer, shared)) {
        return false;
    }
    uint8_t salt[64];
    memcpy(salt, r.data(), 32);
    memcpy(salt + 32, b.data(), 32);
    hkdf32(shared.data(), shared.size(), salt, sizeof salt, _pairwise_info, k.data());
    return true;
}

// AES-256-GCM: ciphertext followed by the 16-byte tag (as Go's crypto/cipher).
static bool seal(const E2E::Key& key, const uint8_t nonce[12], const std::string& aad, const uint8_t* in, size_t len,
                 std::string& out)
{
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    out.assign(len + 16, '\0');
    auto* o  = (uint8_t*)out.data();
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key.data(), 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, nonce, 12, (const uint8_t*)aad.data(), aad.size(),
                                        in, o, 16, o + len) == 0;
    mbedtls_gcm_free(&g);
    return ok;
}

static bool open(const E2E::Key& key, const uint8_t nonce[12], const std::string& aad, const uint8_t* in, size_t len,
                 std::string& out)
{
    if (len < 16) {
        return false;
    }
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    out.assign(len - 16, '\0');
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key.data(), 256) == 0 &&
              mbedtls_gcm_auth_decrypt(&g, len - 16, nonce, 12, (const uint8_t*)aad.data(), aad.size(), in + len - 16, 16,
                                       in, (uint8_t*)out.data()) == 0;
    mbedtls_gcm_free(&g);
    return ok;
}

static std::string browser_id(const E2E::Key& pub)
{
    uint8_t sum[32];
    mbedtls_sha256(pub.data(), pub.size(), sum, 0);
    return hex(sum, 8);
}

static bool key_from_b64(const std::string& s, E2E::Key& k)
{
    std::string raw;
    if (!unb64u(s, raw) || raw.size() != k.size()) {
        return false;
    }
    memcpy(k.data(), raw.data(), k.size());
    return true;
}

static std::string enroll_msg(const E2E::Key& r, const E2E::Key& b)
{
    return std::string(_enroll_label) + b64u(r.data(), r.size()) + "|" + b64u(b.data(), b.size());
}

/* ---------------------------------- E2E ----------------------------------- */

bool E2E::begin(const std::string& robotId)
{
    std::lock_guard<std::mutex> lock(_mu);
    _robot_id = robotId;
    Settings settings("embody_e2e", true);
    if (!key_from_b64(settings.GetString("key", ""), _priv)) {
        esp_fill_random(_priv.data(), _priv.size());
        settings.SetString("key", b64u(_priv.data(), _priv.size()));
    }
    if (!x25519_public(_priv, _pub)) {
        mclog::tagError(_tag, "robot key: X25519 failed");
        return false;
    }
    _browsers.clear();
    _pairwise.clear();
    std::string list = settings.GetString("browsers", "");
    for (size_t start = 0; start < list.size();) {
        size_t end = list.find(',', start);
        if (end == std::string::npos) {
            end = list.size();
        }
        Key b;
        if (key_from_b64(list.substr(start, end - start), b)) {
            add_browser(b);
        }
        start = end + 1;
    }
    _epoch = (uint32_t)settings.GetInt("epoch", 0) + 1;  // a new group key on every start
    settings.SetInt("epoch", (int32_t)_epoch);
    esp_fill_random(_group.data(), _group.size());
    esp_fill_random(_nonce_prefix, sizeof _nonce_prefix);
    _secrets.assign(1, {});
    esp_fill_random(_secrets[0].data(), _secrets[0].size());
    mclog::tagInfo(_tag, "on: epoch {}, {} browsers enrolled", _epoch, _browsers.size());
    return true;
}

void E2E::add_browser(const Key& pub)
{
    Key k;
    if (!pairwise(_priv, pub, _pub, pub, k)) {
        return;
    }
    const auto id = browser_id(pub);
    if (!_pairwise.count(id)) {
        if (_browsers.size() >= _max_browsers) {  // the oldest goes
            _pairwise.erase(browser_id(_browsers.front()));
            _browsers.erase(_browsers.begin());
        }
        _browsers.push_back(pub);
    }
    _pairwise[id] = k;
}

void E2E::save()
{
    std::string list;
    for (const auto& b : _browsers) {
        if (!list.empty()) {
            list.push_back(',');
        }
        list += b64u(b.data(), b.size());
    }
    Settings settings("embody_e2e", true);
    settings.SetString("browsers", list);
}

void E2E::next_nonce(uint8_t out[12])
{
    memcpy(out, _nonce_prefix, 4);
    const uint64_t n = ++_nonce_ctr;
    for (int i = 0; i < 8; i++) {
        out[4 + i] = (uint8_t)(n >> (56 - 8 * i));
    }
}

std::string E2E::fragment()
{
    std::lock_guard<std::mutex> lock(_mu);
    std::array<uint8_t, 16> p;
    esp_fill_random(p.data(), p.size());
    _secrets.insert(_secrets.begin(), p);
    if (_secrets.size() > 2) {
        _secrets.resize(2);
    }
    return "e2e=1." + b64u(_pub.data(), _pub.size()) + "." + b64u(p.data(), p.size());
}

bool E2E::group_key_body(const std::string& browserId, std::string& body)
{
    auto it = _pairwise.find(browserId);
    if (it == _pairwise.end()) {
        return false;
    }
    uint8_t n[12];
    next_nonce(n);
    std::string c;
    const std::string aad = std::string(_group_label) + _robot_id + "|" + std::to_string(_epoch);
    if (!seal(it->second, n, aad, _group.data(), _group.size(), c)) {
        return false;
    }
    ArduinoJson::JsonDocument doc;
    doc["b"]     = browserId;
    doc["epoch"] = _epoch;
    doc["n"]     = b64u(n, sizeof n);
    doc["c"]     = b64u((const uint8_t*)c.data(), c.size());
    body.clear();
    ArduinoJson::serializeJson(doc, body);
    return true;
}

bool E2E::enroll(const std::string& browserPub, const std::string& mac, std::string& groupKeyBody)
{
    std::lock_guard<std::mutex> lock(_mu);
    Key b;
    std::string got;
    if (!key_from_b64(browserPub, b) || !unb64u(mac, got) || got.size() != 32) {
        return false;
    }
    const std::string msg = enroll_msg(_pub, b);
    for (size_t i = 0; i < _secrets.size(); i++) {
        uint8_t want[32];
        hmac(_secrets[i].data(), _secrets[i].size(), (const uint8_t*)msg.data(), msg.size(), want);
        uint8_t diff = 0;
        for (int j = 0; j < 32; j++) {
            diff |= want[j] ^ (uint8_t)got[j];
        }
        if (diff == 0) {
            _secrets.erase(_secrets.begin() + i);  // one secret enrolls one browser
            add_browser(b);
            save();
            mclog::tagInfo(_tag, "browser {} enrolled ({} now)", browser_id(b), _browsers.size());
            return group_key_body(browser_id(b), groupKeyBody);
        }
    }
    mclog::tagWarn(_tag, "enrollment refused: MAC does not match");
    return false;
}

bool E2E::hello(const std::string& browserId, std::string& groupKeyBody)
{
    std::lock_guard<std::mutex> lock(_mu);
    return group_key_body(browserId, groupKeyBody);
}

bool E2E::openCommand(const std::string& browserId, const std::string& n, const std::string& c, std::string& plain)
{
    std::lock_guard<std::mutex> lock(_mu);
    auto it = _pairwise.find(browserId);
    std::string nonce, sealed;
    if (it == _pairwise.end() || !unb64u(n, nonce) || nonce.size() != 12 || !unb64u(c, sealed) ||
        !open(it->second, (const uint8_t*)nonce.data(), _robot_id, (const uint8_t*)sealed.data(), sealed.size(), plain)) {
        return false;
    }
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, plain)) {
        return false;
    }
    const uint64_t seq = doc["seq"] | (uint64_t)0;
    if (seq <= _seqs[browserId]) {
        mclog::tagWarn(_tag, "command refused: replayed sequence");
        return false;
    }
    _seqs[browserId] = seq;
    return true;
}

bool E2E::sealData(const std::string& plainFrame, std::string& body)
{
    std::lock_guard<std::mutex> lock(_mu);
    uint8_t n[12];
    next_nonce(n);
    std::string c;
    if (!seal(_group, n, _robot_id, (const uint8_t*)plainFrame.data(), plainFrame.size(), c)) {
        return false;
    }
    ArduinoJson::JsonDocument doc;
    doc["epoch"] = _epoch;
    doc["n"]     = b64u(n, sizeof n);
    doc["c"]     = b64u((const uint8_t*)c.data(), c.size());
    body.clear();
    ArduinoJson::serializeJson(doc, body);
    return true;
}

bool E2E::sealBinary(uint8_t type, const uint8_t* data, size_t len, std::string& out)
{
    std::lock_guard<std::mutex> lock(_mu);
    uint8_t n[12];
    next_nonce(n);
    std::string plain;
    plain.reserve(len + 1);
    plain.push_back((char)type);
    plain.append((const char*)data, len);
    std::string c;
    if (!seal(_group, n, _robot_id, (const uint8_t*)plain.data(), plain.size(), c)) {
        return false;
    }
    out.clear();
    out.reserve(1 + 4 + 12 + c.size());
    out.push_back((char)0x30);
    for (int i = 3; i >= 0; i--) {
        out.push_back((char)(_epoch >> (8 * i)));
    }
    out.append((const char*)n, sizeof n);
    out += c;
    return true;
}

bool E2E::openBrowserBinary(const std::string& msg, std::string& plain)
{
    std::lock_guard<std::mutex> lock(_mu);
    if (msg.size() < 1 + 8 + 12 + 16 || (uint8_t)msg[0] != 0x31) {
        return false;
    }
    auto it = _pairwise.find(hex((const uint8_t*)msg.data() + 1, 8));
    return it != _pairwise.end() &&
           open(it->second, (const uint8_t*)msg.data() + 9, _robot_id, (const uint8_t*)msg.data() + 21, msg.size() - 21,
                plain);
}

size_t E2E::enrolled() const
{
    std::lock_guard<std::mutex> lock(_mu);
    return _browsers.size();
}

void E2E::forgetAll()
{
    std::lock_guard<std::mutex> lock(_mu);
    _browsers.clear();
    _pairwise.clear();
    _seqs.clear();
    _epoch++;
    esp_fill_random(_group.data(), _group.size());
    Settings settings("embody_e2e", true);
    settings.SetString("browsers", "");
    settings.SetInt("epoch", (int32_t)_epoch);
    mclog::tagInfo(_tag, "all browsers forgotten, epoch {}", _epoch);
}

/* -------------------------------- self-test ------------------------------- */

// The reference test vectors (s-w42-eu-raw e2e/testdata/vectors.json).
bool E2E::selfTest()
{
    int bad      = 0;
    auto check   = [&](const char* what, const std::string& got, const std::string& want) {
        if (got != want) {
            bad++;
            mclog::tagError(_tag, "self-test {}: got {}, want {}", what, got, want);
        }
    };
    Key r, b, rPub, bPub, k;
    r.fill(0x11);
    b.fill(0x22);
    x25519_public(r, rPub);
    x25519_public(b, bPub);
    check("robot pub", b64u(rPub.data(), 32), "e06Qm75__kTEZaIgA31gjuNYl9Me-XLwf3SJLLD3PxM");
    check("browser pub", b64u(bPub.data(), 32), "D6poTtKIZ7l_Smot7l34zpdOdrcBjj8iocTPJnhXDyA");
    check("browser id", browser_id(bPub), "65cf5c9b1de5d41f");
    uint8_t p[16], mac[32];
    memset(p, 0x33, sizeof p);
    const std::string msg = enroll_msg(rPub, bPub);
    hmac(p, sizeof p, (const uint8_t*)msg.data(), msg.size(), mac);
    check("enroll mac", b64u(mac, 32), "8mQu0P33nP2ym7ecp5FbtWgpMAJdL3FFhzpego2zJRY");
    pairwise(r, bPub, rPub, bPub, k);
    check("pairwise", hex(k.data(), 32), "7511e5dda8d78ec928ce24c6e5f7c2a832d4a4256afce45cfbf29d600ef04964");
    Key g;
    g.fill(0x44);
    uint8_t n[12];
    std::string c;
    memset(n, 0x55, sizeof n);
    seal(k, n, std::string(_group_label) + "stackchan-0a1b2c3d4e50|3", g.data(), g.size(), c);
    check("group key", b64u((const uint8_t*)c.data(), c.size()),
          "13Gin-HpdH2MMdKDLwwUC2XuUtm_PNvEaiLBnDIrcvG3mdSB3v6k1GKSz72APZu-");
    memset(n, 0x66, sizeof n);
    const uint8_t frame[] = {0x01, 'f', 'r', 'a', 'm', 'e'};
    seal(g, n, "stackchan-0a1b2c3d4e50", frame, sizeof frame, c);
    check("binary", "3000000003" + hex(n, 12) + hex((const uint8_t*)c.data(), c.size()),
          "3000000003666666666666666666666666ebf588a36eae30c9148987b577cd27717ea1f6879e02");
    memset(n, 0x77, sizeof n);
    const std::string cmd = "{\"command\":\"nod\",\"args\":{},\"seq\":1}";
    seal(k, n, "stackchan-0a1b2c3d4e50", (const uint8_t*)cmd.data(), cmd.size(), c);
    check("command", b64u((const uint8_t*)c.data(), c.size()),
          "Q9ejhcJyPJPuVkNckvypcmXaushVzeuIv0yO3nCRbxbVscZStbZy_sI-eewTWY4Kw3cD");
    std::string plain;
    check("open", open(k, n, "stackchan-0a1b2c3d4e50", (const uint8_t*)c.data(), c.size(), plain) ? plain : "(failed)", cmd);
    if (bad == 0) {
        mclog::tagInfo(_tag, "self-test: all reference vectors match");
    }
    return bad == 0;
}
