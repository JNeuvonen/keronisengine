#pragma once

// C ABI declarations for the Hyperliquid Rust signer shared library
// (hyperliquid-signer-linux-amd64.so).
//
// Source crate: src/rust/packages/hyperliquid_signer. Rebuild with:
//   cd src/rust && cargo build -p hyperliquid_signer --release
//   cp target/release/libhyperliquid_signer.so \
//      ../cpp/packages/common/lib/hyperliquid-signer-linux-amd64.so
//
// Signs a Hyperliquid L1 action (order / cancel): the Rust side msgpack-encodes
// the action, appends nonce + vault byte, keccak-hashes to a connectionId, then
// EIP-712-signs the phantom-agent struct (domain Exchange/v1/chainId 1337).
// mainnet vs testnet differ ONLY by the phantom-agent source ("a" vs "b").

#include <cstdint>

extern "C" {

// One order in a (possibly bulk) order action. Strings are caller-owned and
// must outlive the call. cloid: null or empty -> omitted from the action.
struct HlOrderSpec {
    uint32_t asset;        // perp asset index (from /info meta universe)
    int is_buy;            // 1 buy, 0 sell
    const char* price;     // already exchange-formatted (<=5 sig figs)
    const char* size;      // already exchange-formatted (szDecimals)
    int reduce_only;       // 1 / 0
    const char* tif;       // "Gtc" | "Alo" | "Ioc"
    const char* cloid;     // 128-bit hex (0x...) or null/empty
};

struct HlCancelSpec {
    uint32_t asset;        // perp asset index
    uint64_t oid;          // exchange order id
};

// Result of a sign call. On success err==nullptr and action_json/sig_* are set.
// All non-null char* are Rust-allocated; free the whole struct with HlFree.
struct HlSignedAction {
    char* action_json;     // canonical action JSON to embed in POST /exchange
    char* sig_r;           // 0x-hex
    char* sig_s;           // 0x-hex
    int sig_v;             // 27 or 28
    char* err;             // error string, or nullptr on success
};

// mainnet: 1 -> source "a", 0 -> source "b". vault: null/empty for none.
HlSignedAction SignOrderAction(const char* pk_hex, int mainnet, uint64_t nonce,
                               const char* vault, const HlOrderSpec* orders,
                               int n);

HlSignedAction SignCancelAction(const char* pk_hex, int mainnet, uint64_t nonce,
                                const char* vault, const HlCancelSpec* cancels,
                                int n);

// Frees the Rust-allocated strings held by *resp (safe on a stack value).
void HlFree(HlSignedAction* resp);

}  // extern "C"
