//! Hardware UID, pseudonym binding, Proof-of-Report blacklist (in-memory).
//!
//! UID from stable-ish sources (CPUID, optional TPM material, etc.). Without a
//! TPM you don't get a true per-unit serial. Mesh admission logic is here as a
//! prototype — not a distributed consensus system yet.


use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, Ordering};

/// UID / commitment / pseudonym-hash length.
pub const UID_BYTES: usize = 32;
/// Distinct reporters required to ban a terminal (consensus threshold).
pub const REPORT_THRESHOLD: u8 = 3;

const MAX_BANNED: usize = 64;
const MAX_PENDING: usize = 64;
const MAX_REPORTERS_PER: usize = 8;

// ---------------------------------------------------------------------------
//  Hash primitive
// ---------------------------------------------------------------------------
#[cfg(feature = "pqc")]
fn hash256(parts: &[&[u8]], out: &mut [u8; UID_BYTES]) {
    use sha3::{Digest, Sha3_256};
    let mut h = Sha3_256::new();
    for p in parts {
        h.update(p);
    }
    out.copy_from_slice(&h.finalize());
}

#[cfg(not(feature = "pqc"))]
fn hash256(parts: &[&[u8]], out: &mut [u8; UID_BYTES]) {
    // Non-cryptographic fallback (FNV-1a expanded). Scaffold only.
    let mut acc: u64 = 0xcbf2_9ce4_8422_2325;
    for p in parts {
        for &b in *p {
            acc ^= b as u64;
            acc = acc.wrapping_mul(0x100_0000_01b3);
        }
    }
    for (i, chunk) in out.chunks_mut(8).enumerate() {
        let mut z = acc ^ (i as u64).wrapping_mul(0x9e37_79b9_7f4a_7c15);
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^= z >> 31;
        let w = z.to_le_bytes();
        chunk.copy_from_slice(&w[..chunk.len()]);
    }
}

/// Plain SHA3-256 of `data` (used for `.rgxsite` content addressing/integrity).
pub fn sha3_256(data: &[u8], out: &mut [u8; UID_BYTES]) {
    hash256(&[data], out);
}

/// Derive the Unique Hardware ID from stable hardware sources.
/// Domain-separated so a UID can never collide with a pseudonym commitment.
pub fn derive_uid(tpm_ek: &[u8], cpu_id: &[u8], mac: &[u8], out: &mut [u8; UID_BYTES]) {
    hash256(&[b"ROGEX-UID-v1", tpm_ek, b"|", cpu_id, b"|", mac], out);
}

/// Commit an immutable pseudonym to a UID: `bind = H("BIND" || uid || name)`.
pub fn bind_pseudonym(uid: &[u8; UID_BYTES], name: &[u8], out: &mut [u8; UID_BYTES]) {
    hash256(&[b"ROGEX-BIND-v1", uid, b"|", name], out);
}

// ---------------------------------------------------------------------------
//  Proof-of-Report blacklist
// ---------------------------------------------------------------------------
#[derive(Clone, Copy)]
struct Pending {
    subject: [u8; UID_BYTES],
    reporters: [[u8; UID_BYTES]; MAX_REPORTERS_PER],
    nrep: u8,
    used: bool,
}

struct Laws {
    banned: [[u8; UID_BYTES]; MAX_BANNED],
    nbanned: usize,
    pending: [Pending; MAX_PENDING],
}

/// Result of a report submission (mirrors the C ABI return codes).
#[derive(PartialEq)]
pub enum ReportOutcome {
    AlreadyBanned = -1isize as isize,
    Duplicate = 0,
    Accepted = 1,
    NowBanned = 2,
}

struct SpinCell<T> {
    locked: AtomicBool,
    inner: UnsafeCell<T>,
}
unsafe impl<T> Sync for SpinCell<T> {}
impl<T> SpinCell<T> {
    const fn new(v: T) -> Self {
        SpinCell { locked: AtomicBool::new(false), inner: UnsafeCell::new(v) }
    }
    fn with<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
        while self
            .locked
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        let r = f(unsafe { &mut *self.inner.get() });
        self.locked.store(false, Ordering::Release);
        r
    }
}

static LAWS: SpinCell<Laws> = SpinCell::new(Laws {
    banned: [[0u8; UID_BYTES]; MAX_BANNED],
    nbanned: 0,
    pending: [Pending {
        subject: [0u8; UID_BYTES],
        reporters: [[0u8; UID_BYTES]; MAX_REPORTERS_PER],
        nrep: 0,
        used: false,
    }; MAX_PENDING],
});

fn eq(a: &[u8; UID_BYTES], b: &[u8; UID_BYTES]) -> bool {
    let mut d = 0u8;
    for i in 0..UID_BYTES {
        d |= a[i] ^ b[i];
    }
    d == 0
}

fn is_banned_locked(l: &Laws, uid: &[u8; UID_BYTES]) -> bool {
    l.banned[..l.nbanned].iter().any(|b| eq(b, uid))
}

/// Submit a report. A terminal is banned once `REPORT_THRESHOLD` distinct UIDs
/// report it; a repeat report from the same reporter is ignored.
pub fn report(reporter: &[u8; UID_BYTES], subject: &[u8; UID_BYTES]) -> ReportOutcome {
    LAWS.with(|l| {
        if is_banned_locked(l, subject) {
            return ReportOutcome::AlreadyBanned;
        }
        // Find or allocate the pending entry for `subject`.
        let mut slot = usize::MAX;
        for i in 0..MAX_PENDING {
            if l.pending[i].used && eq(&l.pending[i].subject, subject) {
                slot = i;
                break;
            }
        }
        if slot == usize::MAX {
            for i in 0..MAX_PENDING {
                if !l.pending[i].used {
                    l.pending[i].used = true;
                    l.pending[i].subject = *subject;
                    l.pending[i].nrep = 0;
                    slot = i;
                    break;
                }
            }
        }
        if slot == usize::MAX {
            return ReportOutcome::Accepted; // table full: drop silently
        }
        // Reject a duplicate reporter.
        let n = l.pending[slot].nrep as usize;
        for i in 0..n {
            if eq(&l.pending[slot].reporters[i], reporter) {
                return ReportOutcome::Duplicate;
            }
        }
        if n < MAX_REPORTERS_PER {
            l.pending[slot].reporters[n] = *reporter;
            l.pending[slot].nrep += 1;
        }
        if l.pending[slot].nrep >= REPORT_THRESHOLD && l.nbanned < MAX_BANNED {
            l.banned[l.nbanned] = *subject;
            l.nbanned += 1;
            l.pending[slot].used = false; // free the pending slot
            return ReportOutcome::NowBanned;
        }
        ReportOutcome::Accepted
    })
}

pub fn is_banned(uid: &[u8; UID_BYTES]) -> bool {
    LAWS.with(|l| is_banned_locked(l, uid))
}

/// Mesh admission control: banned terminals are denied.
pub fn admit(uid: &[u8; UID_BYTES]) -> bool {
    !is_banned(uid)
}

pub fn banned_count() -> usize {
    LAWS.with(|l| l.nbanned)
}
