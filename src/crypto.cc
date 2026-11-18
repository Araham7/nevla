// Field encryption: AES-256-GCM (OpenSSL). Stored as text "nvenc:1:" + base64(iv[12] | ciphertext | tag[16]).
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>

#include "nevla/plugins.hpp"

namespace nevla {
namespace {

const std::string kPrefix = "nvenc:1:";
using Key = std::array<unsigned char, 32>;

std::mutex gKeyMu;
std::shared_ptr<const Key> gKey;
std::atomic<bool> gHasKey{false};

std::shared_ptr<const Key> key() {
    std::lock_guard<std::mutex> lk(gKeyMu);
    if (!gKey) throw Error("nevla: an encrypted() field needs a key: call nevla::setEncryptionKey(...) first");
    return gKey;
}

const char *kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const unsigned char *data, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < n ? kB64[v & 63] : '=';
    }
    return out;
}

bool unbase64(const std::string &in, std::string &out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (in.size() % 4) return false;
    out.clear();
    for (size_t i = 0; i < in.size(); i += 4) {
        int a = val(in[i]), b = val(in[i + 1]);
        int c = in[i + 2] == '=' ? 0 : val(in[i + 2]), d = in[i + 3] == '=' ? 0 : val(in[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        uint32_t v = (a << 18) | (b << 12) | (c << 6) | d;
        out += static_cast<char>((v >> 16) & 255);
        if (in[i + 2] != '=') out += static_cast<char>((v >> 8) & 255);
        if (in[i + 3] != '=') out += static_cast<char>(v & 255);
    }
    return true;
}

struct CtxFree {
    void operator()(EVP_CIPHER_CTX *c) const { EVP_CIPHER_CTX_free(c); }
};

}  // namespace

void setEncryptionKey(const std::string &k) {
    if (k.empty()) throw Error("nevla: setEncryptionKey(): the key is empty");
    auto out = std::make_shared<Key>();
    bool hex = k.size() == 64;
    for (char c : k) hex &= std::isxdigit(static_cast<unsigned char>(c)) != 0;
    if (hex) {
        for (size_t i = 0; i < 32; i++)
            (*out)[i] = static_cast<unsigned char>(std::stoi(k.substr(i * 2, 2), nullptr, 16));
    } else {  // passphrase -> SHA-256
        unsigned int len = 0;
        if (!EVP_Digest(k.data(), k.size(), out->data(), &len, EVP_sha256(), nullptr) || len != 32)
            throw Error("nevla: setEncryptionKey(): SHA-256 failed");
    }
    std::lock_guard<std::mutex> lk(gKeyMu);
    gKey = std::move(out);
    gHasKey = true;
}

void clearEncryptionKey() {
    std::lock_guard<std::mutex> lk(gKeyMu);
    gKey.reset();
    gHasKey = false;
}

bool detail::hasEncryptionKey() { return gHasKey.load(); }

bool detail::isEncryptedText(const std::string &s) { return s.compare(0, kPrefix.size(), kPrefix) == 0; }

std::string detail::encryptText(const std::string &plain, bool deterministic) {
    if (isEncryptedText(plain)) return plain;  // already encrypted
    auto k = key();
    unsigned char iv[12];
    if (deterministic) {  // synthetic IV: HMAC(key, text) -> same text, same ciphertext
        unsigned char mac[32];
        unsigned int macLen = 0;
        std::string msg = "nevla-iv:" + plain;
        if (!HMAC(EVP_sha256(), k->data(), 32, reinterpret_cast<const unsigned char *>(msg.data()), msg.size(), mac,
                  &macLen))
            throw Error("nevla: encryption failed (HMAC)");
        std::copy(mac, mac + 12, iv);
    } else if (RAND_bytes(iv, sizeof iv) != 1) {
        throw Error("nevla: encryption failed (random IV)");
    }
    std::unique_ptr<EVP_CIPHER_CTX, CtxFree> ctx(EVP_CIPHER_CTX_new());
    std::string buf(12 + plain.size() + 16, '\0');
    auto *out = reinterpret_cast<unsigned char *>(buf.data());
    std::copy(iv, iv + 12, out);
    int len = 0, total = 0;
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, k->data(), iv) != 1 ||
        EVP_EncryptUpdate(ctx.get(), out + 12, &len, reinterpret_cast<const unsigned char *>(plain.data()),
                          static_cast<int>(plain.size())) != 1)
        throw Error("nevla: encryption failed");
    total = len;
    if (EVP_EncryptFinal_ex(ctx.get(), out + 12 + total, &len) != 1) throw Error("nevla: encryption failed");
    total += len;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, out + 12 + total) != 1)
        throw Error("nevla: encryption failed (tag)");
    return kPrefix + base64(out, 12 + total + 16);
}

std::string detail::decryptText(const std::string &cipher) {
    if (!isEncryptedText(cipher)) return cipher;
    auto k = key();
    std::string raw;
    if (!unbase64(cipher.substr(kPrefix.size()), raw) || raw.size() < 28)
        throw Error("nevla: an encrypted value is damaged");
    auto *in = reinterpret_cast<const unsigned char *>(raw.data());
    int n = static_cast<int>(raw.size()) - 28;
    std::string plain(static_cast<size_t>(n), '\0');
    std::unique_ptr<EVP_CIPHER_CTX, CtxFree> ctx(EVP_CIPHER_CTX_new());
    int len = 0, total = 0;
    unsigned char tag[16];
    std::copy(in + 12 + n, in + 12 + n + 16, tag);
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, k->data(), in) != 1 ||
        EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(plain.data()), &len, in + 12, n) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, tag) != 1)
        throw Error("nevla: could not decrypt a value");
    total = len;
    if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(plain.data()) + total, &len) != 1)
        throw Error("nevla: could not decrypt a value (wrong encryption key?)");
    total += len;
    plain.resize(static_cast<size_t>(total));
    return plain;
}

}  // namespace nevla
