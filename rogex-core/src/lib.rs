//! rogex-core — `no_std` crypto/mesh bits for RXos.
//!
//! Built for `x86_64-unknown-none`, linked into the C kernel.
//! Owns the PQ handshake (ML-KEM family) and a thin mesh scaffold.
//! C calls the `extern "C"` surface in `kernel/rogex_core.h`.
//!
//! Research code. Self-tests at boot; no external audit yet.


#![no_std]
#![allow(clippy::missing_safety_doc)]

use core::panic::PanicInfo;
use core::sync::atomic::{AtomicU64, Ordering};

pub mod aead;
pub mod crypto;
pub mod identity;
pub mod mesh;

/// Write one byte to the machine's console, so bare-metal Rust panics are
/// not silent. Which device that is belongs to the architecture: COM1 on
/// x86, the PL011 on the AArch64 boards this runs on. A panic path that
/// only works on one of them is a panic path that will be missing on the
/// day it matters.
#[cfg(target_arch = "x86_64")]
unsafe fn console_putc(b: u8) {
    loop {
        let lsr: u8;
        core::arch::asm!("in al, dx", out("al") lsr, in("dx") 0x3fdu16,
                         options(nomem, nostack, preserves_flags));
        if lsr & 0x20 != 0 {
            break;
        }
    }
    core::arch::asm!("out dx, al", in("dx") 0x3f8u16, in("al") b,
                     options(nomem, nostack, preserves_flags));
}

#[cfg(target_arch = "aarch64")]
unsafe fn console_putc(b: u8) {
    // PL011 at the QEMU virt address: data register at +0x00, flag register
    // at +0x18 with TXFF in bit 5.
    const PL011: usize = 0x0900_0000;
    let fr = (PL011 + 0x18) as *const u32;
    let dr = PL011 as *mut u32;
    while core::ptr::read_volatile(fr) & (1 << 5) != 0 {}
    core::ptr::write_volatile(dr, b as u32);
}

/// Minimal COM1 sink so we can format the panic message on bare metal.
struct SerialWriter;
impl core::fmt::Write for SerialWriter {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for &b in s.as_bytes() {
            unsafe { console_putc(b) };
        }
        Ok(())
    }
}

/// On bare metal there is nothing to unwind into: report to serial, then halt.
#[panic_handler]
fn on_panic(info: &PanicInfo) -> ! {
    use core::fmt::Write;
    let _ = write!(SerialWriter, "\n[rogex] RUST PANIC: {}\n", info);
    loop {
        // SAFETY: halting the CPU.
        #[cfg(target_arch = "x86_64")]
        unsafe { core::arch::asm!("cli; hlt", options(nomem, nostack)) };
        #[cfg(target_arch = "aarch64")]
        unsafe { core::arch::asm!("msr daifset, #2; wfe", options(nomem, nostack)) };
    }
}

/// Process-wide RNG state (xorshift128+ style). Seeded by [`rogex_core_init`].
static RNG_STATE: AtomicU64 = AtomicU64::new(0x9e3779b97f4a7c15);

/// Pull the next 64-bit value from the core RNG.
///
/// This is a splitmix64 stream. On real hardware, reseed/augment it from
/// `RDRAND`/`RDSEED` and the TPM RNG — see [`crypto`] for where entropy feeds
/// the KEM. A PRNG alone is never sufficient for key generation in production.
pub(crate) fn rng_next() -> u64 {
    let mut z = RNG_STATE.fetch_add(0x9e3779b97f4a7c15, Ordering::Relaxed) + 0x9e3779b97f4a7c15;
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
    z ^ (z >> 31)
}

/// Fill a byte slice from the core RNG.
pub(crate) fn rng_fill(out: &mut [u8]) {
    let mut i = 0;
    while i < out.len() {
        let w = rng_next().to_le_bytes();
        let n = core::cmp::min(8, out.len() - i);
        out[i..i + n].copy_from_slice(&w[..n]);
        i += n;
    }
}

// ===========================================================================
//  C ABI surface (see kernel/rogex_core.h)
// ===========================================================================

/// Initialize the core. `seed` should be a high-entropy value (e.g. TSC mixed
/// with RDRAND). Returns 0 on success.
#[no_mangle]
pub extern "C" fn rogex_core_init(seed: u64) -> i32 {
    let s = seed ^ 0x2545_f491_4f6c_dd1d;
    RNG_STATE.store(if s == 0 { 0x1234_5678_9abc_def0 } else { s }, Ordering::Relaxed);
    mesh::init();
    0
}

/// ML-KEM key generation. `pk`/`sk` must point to buffers of the sizes in
/// `rogex_core.h` (public/secret key). Returns 0 on success.
#[no_mangle]
pub unsafe extern "C" fn rogex_kem_keypair(pk: *mut u8, sk: *mut u8) -> i32 {
    if pk.is_null() || sk.is_null() {
        return -1;
    }
    let pk = core::slice::from_raw_parts_mut(pk, crypto::PUBLICKEY_BYTES);
    let sk = core::slice::from_raw_parts_mut(sk, crypto::SECRETKEY_BYTES);
    crypto::keypair(pk, sk);
    0
}

/// ML-KEM encapsulation: derive a shared secret `ss` and its ciphertext `ct`
/// for the holder of `pk`. Returns 0 on success.
#[no_mangle]
pub unsafe extern "C" fn rogex_kem_encapsulate(pk: *const u8, ct: *mut u8, ss: *mut u8) -> i32 {
    if pk.is_null() || ct.is_null() || ss.is_null() {
        return -1;
    }
    let pk = core::slice::from_raw_parts(pk, crypto::PUBLICKEY_BYTES);
    let ct = core::slice::from_raw_parts_mut(ct, crypto::CIPHERTEXT_BYTES);
    let ss = core::slice::from_raw_parts_mut(ss, crypto::SHAREDSECRET_BYTES);
    crypto::encapsulate(pk, ct, ss);
    0
}

/// ML-KEM decapsulation: recover the shared secret `ss` from `ct` using `sk`.
/// Returns 0 on success.
#[no_mangle]
pub unsafe extern "C" fn rogex_kem_decapsulate(sk: *const u8, ct: *const u8, ss: *mut u8) -> i32 {
    if sk.is_null() || ct.is_null() || ss.is_null() {
        return -1;
    }
    let sk = core::slice::from_raw_parts(sk, crypto::SECRETKEY_BYTES);
    let ct = core::slice::from_raw_parts(ct, crypto::CIPHERTEXT_BYTES);
    let ss = core::slice::from_raw_parts_mut(ss, crypto::SHAREDSECRET_BYTES);
    crypto::decapsulate(sk, ct, ss);
    0
}

/// Enqueue a datagram onto the mesh transmit path. Non-blocking. Returns the
/// number of bytes accepted, or a negative error.
#[no_mangle]
pub unsafe extern "C" fn rogex_mesh_tx(buf: *const u8, len: u32) -> i32 {
    if buf.is_null() {
        return -1;
    }
    let data = core::slice::from_raw_parts(buf, len as usize);
    mesh::tx(data) as i32
}

// --- ChaCha20-Poly1305 AEAD: seal the REVM bytecode under the ML-KEM secret --

/// Fill `buf` with `len` bytes from the core PRNG (e.g. an AEAD nonce).
#[no_mangle]
pub unsafe extern "C" fn rogex_random_bytes(buf: *mut u8, len: u32) {
    if buf.is_null() {
        return;
    }
    rng_fill(core::slice::from_raw_parts_mut(buf, len as usize));
}

/// Seal `buf` (len bytes) in place under `key` (32 B) + `nonce` (12 B); the
/// 16-byte authentication tag is written to `tag`. AAD is empty.
#[no_mangle]
pub unsafe extern "C" fn rogex_aead_seal(
    key: *const u8, nonce: *const u8, buf: *mut u8, len: u32, tag: *mut u8,
) -> i32 {
    if key.is_null() || nonce.is_null() || buf.is_null() || tag.is_null() {
        return -1;
    }
    let k: &[u8; 32] = &*(key as *const [u8; 32]);
    let n: &[u8; 12] = &*(nonce as *const [u8; 12]);
    let data = core::slice::from_raw_parts_mut(buf, len as usize);
    let mut t = [0u8; 16];
    aead::seal(k, n, &[], data, &mut t);
    core::ptr::copy_nonoverlapping(t.as_ptr(), tag, 16);
    0
}

/// Verify `tag` over `buf` (len bytes) and, if valid, decrypt `buf` in place.
/// Returns 0 on success, -1 on authentication failure (buf unchanged).
#[no_mangle]
pub unsafe extern "C" fn rogex_aead_open(
    key: *const u8, nonce: *const u8, buf: *mut u8, len: u32, tag: *const u8,
) -> i32 {
    if key.is_null() || nonce.is_null() || buf.is_null() || tag.is_null() {
        return -1;
    }
    let k: &[u8; 32] = &*(key as *const [u8; 32]);
    let n: &[u8; 12] = &*(nonce as *const [u8; 12]);
    let data = core::slice::from_raw_parts_mut(buf, len as usize);
    let t = core::slice::from_raw_parts(tag, 16);
    if aead::open(k, n, &[], data, t) {
        0
    } else {
        -1
    }
}

/// RFC 8439 §2.8.2 known-answer self-test for the AEAD. Returns 0 if correct.
#[no_mangle]
pub extern "C" fn rogex_aead_selftest() -> i32 {
    if aead::selftest() {
        0
    } else {
        -1
    }
}

// --- Phase 2: hardware identity + system of laws -------------------------

/// Derive the 32-byte Unique Hardware ID from TPM/CPU/MAC material.
#[no_mangle]
pub unsafe extern "C" fn rogex_uid_derive(
    tpm: *const u8, tpm_len: u32, cpu: *const u8, cpu_len: u32,
    mac: *const u8, mac_len: u32, out: *mut u8,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    let s = |p: *const u8, n: u32| -> &'static [u8] {
        if p.is_null() { &[] } else { core::slice::from_raw_parts(p, n as usize) }
    };
    let mut uid = [0u8; identity::UID_BYTES];
    identity::derive_uid(s(tpm, tpm_len), s(cpu, cpu_len), s(mac, mac_len), &mut uid);
    core::ptr::copy_nonoverlapping(uid.as_ptr(), out, identity::UID_BYTES);
    0
}

/// Commit an immutable pseudonym to a UID; writes the 32-byte binding.
#[no_mangle]
pub unsafe extern "C" fn rogex_pseudonym_bind(
    uid: *const u8, name: *const u8, name_len: u32, out: *mut u8,
) -> i32 {
    if uid.is_null() || name.is_null() || out.is_null() {
        return -1;
    }
    let u: &[u8; 32] = &*(uid as *const [u8; 32]);
    let n = core::slice::from_raw_parts(name, name_len as usize);
    let mut b = [0u8; identity::UID_BYTES];
    identity::bind_pseudonym(u, n, &mut b);
    core::ptr::copy_nonoverlapping(b.as_ptr(), out, identity::UID_BYTES);
    0
}

/// Submit a Proof-of-Report vote. Returns: 2 now-banned, 1 accepted,
/// 0 duplicate reporter, -1 already banned.
#[no_mangle]
pub unsafe extern "C" fn rogex_report(reporter: *const u8, subject: *const u8) -> i32 {
    if reporter.is_null() || subject.is_null() {
        return -1;
    }
    let r: &[u8; 32] = &*(reporter as *const [u8; 32]);
    let s: &[u8; 32] = &*(subject as *const [u8; 32]);
    match identity::report(r, s) {
        identity::ReportOutcome::NowBanned => 2,
        identity::ReportOutcome::Accepted => 1,
        identity::ReportOutcome::Duplicate => 0,
        identity::ReportOutcome::AlreadyBanned => -1,
    }
}

/// 1 if the UID is on the blacklist, else 0.
#[no_mangle]
pub unsafe extern "C" fn rogex_is_banned(uid: *const u8) -> i32 {
    if uid.is_null() {
        return 0;
    }
    let u: &[u8; 32] = &*(uid as *const [u8; 32]);
    if identity::is_banned(u) { 1 } else { 0 }
}

/// Mesh admission: 1 if the UID may join the mesh, 0 if banned.
#[no_mangle]
pub unsafe extern "C" fn rogex_mesh_admit(uid: *const u8) -> i32 {
    if uid.is_null() {
        return 0;
    }
    let u: &[u8; 32] = &*(uid as *const [u8; 32]);
    if identity::admit(u) { 1 } else { 0 }
}

/// Number of terminals currently blacklisted.
#[no_mangle]
pub extern "C" fn rogex_blacklist_count() -> i32 {
    identity::banned_count() as i32
}

/// SHA3-256 of `data` into `out` (32 bytes). Used to content-address and
/// integrity-check `.rgxsite` assets. Returns 0 on success.
#[no_mangle]
pub unsafe extern "C" fn rogex_sha3_256(data: *const u8, len: u32, out: *mut u8) -> i32 {
    if out.is_null() {
        return -1;
    }
    let d = if data.is_null() { &[][..] } else { core::slice::from_raw_parts(data, len as usize) };
    let mut h = [0u8; 32];
    identity::sha3_256(d, &mut h);
    core::ptr::copy_nonoverlapping(h.as_ptr(), out, 32);
    0
}
