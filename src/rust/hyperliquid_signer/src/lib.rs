//! Hyperliquid L1-action signer, exposed over a C ABI for the C++ trading
//! engine. Mirrors the `lighter-signer` FFI pattern.
//!
//! Signing scheme (matches the official Hyperliquid Python/Rust SDKs):
//!   connectionId = keccak256( msgpack(action) ++ nonce_be8 ++ vault_byte[ ++ vault20] )
//!   digest       = EIP-712 over Agent{source, connectionId}
//!                  domain = {name:"Exchange", version:"1", chainId:1337,
//!                            verifyingContract: 0x0}
//!   mainnet/testnet differ ONLY by source = "a"/"b".
//! The signed digest is recoverable-ECDSA (low-S, v in {27,28}).

use std::os::raw::{c_char, c_int};
use std::ffi::{CStr, CString};
use std::ptr;

use k256::ecdsa::{RecoveryId, Signature, SigningKey};
use serde::Serialize;
use tiny_keccak::{Hasher, Keccak};

// ── Wire structs (field order is load-bearing for msgpack) ─────────────────

#[derive(Serialize)]
struct LimitTif {
    tif: String,
}

#[derive(Serialize)]
struct OrderTypeWire {
    limit: LimitTif,
}

#[derive(Serialize)]
struct OrderWire {
    a: u32,
    b: bool,
    p: String,
    s: String,
    r: bool,
    t: OrderTypeWire,
    #[serde(skip_serializing_if = "Option::is_none")]
    c: Option<String>,
}

#[derive(Serialize)]
struct OrderAction {
    #[serde(rename = "type")]
    typ: String,
    orders: Vec<OrderWire>,
    grouping: String,
}

#[derive(Serialize)]
struct CancelWire {
    a: u32,
    o: u64,
}

#[derive(Serialize)]
struct CancelAction {
    #[serde(rename = "type")]
    typ: String,
    cancels: Vec<CancelWire>,
}

// ── Pure-Rust signing core (also used directly by unit tests) ──────────────

struct OrderInput {
    asset: u32,
    is_buy: bool,
    price: String,
    size: String,
    reduce_only: bool,
    tif: String,
    cloid: Option<String>,
}

fn keccak256(data: &[u8]) -> [u8; 32] {
    let mut k = Keccak::v256();
    let mut out = [0u8; 32];
    k.update(data);
    k.finalize(&mut out);
    out
}

fn action_hash<T: Serialize>(
    action: &T,
    vault: Option<&str>,
    nonce: u64,
) -> Result<[u8; 32], String> {
    let mut data = rmp_serde::to_vec_named(action).map_err(|e| e.to_string())?;
    data.extend_from_slice(&nonce.to_be_bytes());
    match vault {
        None => data.push(0u8),
        Some(v) => {
            data.push(1u8);
            let bytes = hex::decode(v.trim_start_matches("0x"))
                .map_err(|e| format!("bad vault address: {e}"))?;
            data.extend_from_slice(&bytes);
        }
    }
    Ok(keccak256(&data))
}

/// EIP-712 digest for Agent{source, connectionId} under the Exchange domain.
fn eip712_agent_digest(source: &str, connection_id: &[u8; 32]) -> [u8; 32] {
    let domain_type_hash = keccak256(
        b"EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)",
    );
    let name_hash = keccak256(b"Exchange");
    let version_hash = keccak256(b"1");
    let mut chain_id = [0u8; 32];
    chain_id[24..32].copy_from_slice(&1337u64.to_be_bytes());
    let verifying_contract = [0u8; 32]; // address(0), left-padded

    let mut domain_buf = Vec::with_capacity(32 * 5);
    domain_buf.extend_from_slice(&domain_type_hash);
    domain_buf.extend_from_slice(&name_hash);
    domain_buf.extend_from_slice(&version_hash);
    domain_buf.extend_from_slice(&chain_id);
    domain_buf.extend_from_slice(&verifying_contract);
    let domain_separator = keccak256(&domain_buf);

    let agent_type_hash = keccak256(b"Agent(string source,bytes32 connectionId)");
    let source_hash = keccak256(source.as_bytes());
    let mut struct_buf = Vec::with_capacity(32 * 3);
    struct_buf.extend_from_slice(&agent_type_hash);
    struct_buf.extend_from_slice(&source_hash);
    struct_buf.extend_from_slice(connection_id);
    let struct_hash = keccak256(&struct_buf);

    let mut digest_buf = Vec::with_capacity(2 + 64);
    digest_buf.push(0x19);
    digest_buf.push(0x01);
    digest_buf.extend_from_slice(&domain_separator);
    digest_buf.extend_from_slice(&struct_hash);
    keccak256(&digest_buf)
}

struct SignedAction {
    action_json: String,
    r: String,
    s: String,
    v: i32,
}

fn sign_digest(pk_hex: &str, mainnet: bool, connection_id: &[u8; 32]) -> Result<(String, String, i32), String> {
    let source = if mainnet { "a" } else { "b" };
    let digest = eip712_agent_digest(source, connection_id);
    let pk_bytes = hex::decode(pk_hex.trim_start_matches("0x"))
        .map_err(|e| format!("bad private key hex: {e}"))?;
    let sk = SigningKey::from_slice(&pk_bytes).map_err(|e| e.to_string())?;
    let (sig, recid): (Signature, RecoveryId) =
        sk.sign_prehash_recoverable(&digest).map_err(|e| e.to_string())?;
    let r = format!("0x{}", hex::encode(sig.r().to_bytes()));
    let s = format!("0x{}", hex::encode(sig.s().to_bytes()));
    let v = recid.to_byte() as i32 + 27;
    Ok((r, s, v))
}

fn sign_order_action_internal(
    pk_hex: &str,
    mainnet: bool,
    nonce: u64,
    vault: Option<&str>,
    orders: &[OrderInput],
) -> Result<SignedAction, String> {
    if orders.is_empty() {
        return Err("no orders".into());
    }
    let wires: Vec<OrderWire> = orders
        .iter()
        .map(|o| OrderWire {
            a: o.asset,
            b: o.is_buy,
            p: o.price.clone(),
            s: o.size.clone(),
            r: o.reduce_only,
            t: OrderTypeWire {
                limit: LimitTif { tif: o.tif.clone() },
            },
            c: o.cloid.clone(),
        })
        .collect();
    let action = OrderAction {
        typ: "order".into(),
        orders: wires,
        grouping: "na".into(),
    };
    let conn_id = action_hash(&action, vault, nonce)?;
    let (r, s, v) = sign_digest(pk_hex, mainnet, &conn_id)?;
    let action_json = serde_json::to_string(&action).map_err(|e| e.to_string())?;
    Ok(SignedAction { action_json, r, s, v })
}

fn sign_cancel_action_internal(
    pk_hex: &str,
    mainnet: bool,
    nonce: u64,
    vault: Option<&str>,
    cancels: &[CancelWire],
) -> Result<SignedAction, String> {
    if cancels.is_empty() {
        return Err("no cancels".into());
    }
    let action = CancelAction {
        typ: "cancel".into(),
        cancels: cancels.iter().map(|c| CancelWire { a: c.a, o: c.o }).collect(),
    };
    let conn_id = action_hash(&action, vault, nonce)?;
    let (r, s, v) = sign_digest(pk_hex, mainnet, &conn_id)?;
    let action_json = serde_json::to_string(&action).map_err(|e| e.to_string())?;
    Ok(SignedAction { action_json, r, s, v })
}

// ── C ABI ──────────────────────────────────────────────────────────────────

#[repr(C)]
pub struct HlOrderSpec {
    pub asset: u32,
    pub is_buy: c_int,
    pub price: *const c_char,
    pub size: *const c_char,
    pub reduce_only: c_int,
    pub tif: *const c_char,
    pub cloid: *const c_char,
}

#[repr(C)]
pub struct HlCancelSpec {
    pub asset: u32,
    pub oid: u64,
}

#[repr(C)]
pub struct HlSignedAction {
    pub action_json: *mut c_char,
    pub sig_r: *mut c_char,
    pub sig_s: *mut c_char,
    pub sig_v: c_int,
    pub err: *mut c_char,
}

fn ok_resp(s: SignedAction) -> HlSignedAction {
    HlSignedAction {
        action_json: into_raw(s.action_json),
        sig_r: into_raw(s.r),
        sig_s: into_raw(s.s),
        sig_v: s.v,
        err: ptr::null_mut(),
    }
}

fn err_resp(msg: String) -> HlSignedAction {
    HlSignedAction {
        action_json: ptr::null_mut(),
        sig_r: ptr::null_mut(),
        sig_s: ptr::null_mut(),
        sig_v: 0,
        err: into_raw(msg),
    }
}

fn into_raw(s: String) -> *mut c_char {
    match CString::new(s) {
        Ok(c) => c.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}

unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        None
    } else {
        CStr::from_ptr(p).to_str().ok()
    }
}

unsafe fn opt_nonempty<'a>(p: *const c_char) -> Option<&'a str> {
    cstr(p).filter(|s| !s.is_empty())
}

#[no_mangle]
pub extern "C" fn SignOrderAction(
    pk_hex: *const c_char,
    mainnet: c_int,
    nonce: u64,
    vault: *const c_char,
    orders: *const HlOrderSpec,
    n: c_int,
) -> HlSignedAction {
    let result = std::panic::catch_unwind(|| unsafe {
        let pk = match cstr(pk_hex) {
            Some(p) => p,
            None => return Err("null private key".to_string()),
        };
        if orders.is_null() || n <= 0 {
            return Err("no orders".to_string());
        }
        let specs = std::slice::from_raw_parts(orders, n as usize);
        let mut inputs = Vec::with_capacity(specs.len());
        for spec in specs {
            let price = cstr(spec.price).ok_or("null price")?.to_string();
            let size = cstr(spec.size).ok_or("null size")?.to_string();
            let tif = cstr(spec.tif).ok_or("null tif")?.to_string();
            inputs.push(OrderInput {
                asset: spec.asset,
                is_buy: spec.is_buy != 0,
                price,
                size,
                reduce_only: spec.reduce_only != 0,
                tif,
                cloid: opt_nonempty(spec.cloid).map(|s| s.to_string()),
            });
        }
        sign_order_action_internal(pk, mainnet != 0, nonce, opt_nonempty(vault), &inputs)
    });
    match result {
        Ok(Ok(signed)) => ok_resp(signed),
        Ok(Err(e)) => err_resp(e),
        Err(_) => err_resp("panic in SignOrderAction".to_string()),
    }
}

#[no_mangle]
pub extern "C" fn SignCancelAction(
    pk_hex: *const c_char,
    mainnet: c_int,
    nonce: u64,
    vault: *const c_char,
    cancels: *const HlCancelSpec,
    n: c_int,
) -> HlSignedAction {
    let result = std::panic::catch_unwind(|| unsafe {
        let pk = match cstr(pk_hex) {
            Some(p) => p,
            None => return Err("null private key".to_string()),
        };
        if cancels.is_null() || n <= 0 {
            return Err("no cancels".to_string());
        }
        let specs = std::slice::from_raw_parts(cancels, n as usize);
        let wires: Vec<CancelWire> =
            specs.iter().map(|c| CancelWire { a: c.asset, o: c.oid }).collect();
        sign_cancel_action_internal(pk, mainnet != 0, nonce, opt_nonempty(vault), &wires)
    });
    match result {
        Ok(Ok(signed)) => ok_resp(signed),
        Ok(Err(e)) => err_resp(e),
        Err(_) => err_resp("panic in SignCancelAction".to_string()),
    }
}

#[no_mangle]
pub extern "C" fn HlFree(resp: *mut HlSignedAction) {
    if resp.is_null() {
        return;
    }
    unsafe {
        let r = &mut *resp;
        for p in [r.action_json, r.sig_r, r.sig_s, r.err] {
            if !p.is_null() {
                drop(CString::from_raw(p));
            }
        }
        r.action_json = ptr::null_mut();
        r.sig_r = ptr::null_mut();
        r.sig_s = ptr::null_mut();
        r.err = ptr::null_mut();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Hex-normalize: strip 0x and leading zeros for big-int comparison, since
    // the SDK vectors drop leading zero nibbles while k256 emits fixed 32 bytes.
    fn norm(h: &str) -> String {
        h.trim_start_matches("0x").trim_start_matches('0').to_string()
    }

    const PK: &str = "0x0123456789012345678901234567890123456789012345678901234567890123";

    // Official Hyperliquid Python SDK vector (tests/signing_test.py):
    // order ETH(asset=1) buy sz=100 px=100 GTC, nonce=0, vault=None.
    #[test]
    fn order_parity_mainnet() {
        let orders = vec![OrderInput {
            asset: 1,
            is_buy: true,
            price: "100".into(),
            size: "100".into(),
            reduce_only: false,
            tif: "Gtc".into(),
            cloid: None,
        }];
        let s = sign_order_action_internal(PK, true, 0, None, &orders).unwrap();
        assert_eq!(
            norm(&s.r),
            norm("0xd65369825a9df5d80099e513cce430311d7d26ddf477f5b3a33d2806b100d78e")
        );
        assert_eq!(
            norm(&s.s),
            norm("0x2b54116ff64054968aa237c20ca9ff68000f977c93289157748a3162b6ea940e")
        );
        assert_eq!(s.v, 28);
    }

    #[test]
    fn order_parity_testnet() {
        let orders = vec![OrderInput {
            asset: 1,
            is_buy: true,
            price: "100".into(),
            size: "100".into(),
            reduce_only: false,
            tif: "Gtc".into(),
            cloid: None,
        }];
        let s = sign_order_action_internal(PK, false, 0, None, &orders).unwrap();
        assert_eq!(
            norm(&s.r),
            norm("0x82b2ba28e76b3d761093aaded1b1cdad4960b3af30212b343fb2e6cdfa4e3d54")
        );
        assert_eq!(
            norm(&s.s),
            norm("0x6b53878fc99d26047f4d7e8c90eb98955a109f44209163f52d8dc4278cbbd9f5")
        );
        assert_eq!(s.v, 27);
    }

    #[test]
    fn order_action_json_shape() {
        let orders = vec![OrderInput {
            asset: 1,
            is_buy: true,
            price: "100".into(),
            size: "100".into(),
            reduce_only: false,
            tif: "Gtc".into(),
            cloid: None,
        }];
        let s = sign_order_action_internal(PK, true, 0, None, &orders).unwrap();
        assert_eq!(
            s.action_json,
            r#"{"type":"order","orders":[{"a":1,"b":true,"p":"100","s":"100","r":false,"t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"}"#
        );
    }

    #[test]
    fn cloid_included_when_present() {
        let orders = vec![OrderInput {
            asset: 1,
            is_buy: true,
            price: "100".into(),
            size: "100".into(),
            reduce_only: false,
            tif: "Gtc".into(),
            cloid: Some("0x00000000000000000000000000000001".into()),
        }];
        let s = sign_order_action_internal(PK, true, 0, None, &orders).unwrap();
        assert!(s.action_json.contains(r#""c":"0x00000000000000000000000000000001""#));
    }
}
