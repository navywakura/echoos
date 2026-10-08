//! ML-KEM / Kyber-family KEM (FIPS 203 sizes).
//!
//! Default `pqc` feature → real ML-KEM-768 via RustCrypto `ml-kem`.
//! `--no-default-features` → deterministic scaffold so offline builds still
//! link. Scaffold is **not** secure — obvious on purpose.
//!
//! RNG for keygen is the core PRNG; real hardware should mix RDRAND/RDSEED/TPM.


/// ML-KEM-768 public (encapsulation) key length (bytes) — wire-standard.
pub const PUBLICKEY_BYTES: usize = 1184;
/// Secret (decapsulation) key length (bytes). This is the RustCrypto `ml-kem`
/// portable **seed encoding** (`d‖z`, 64 B) that `KeyExport`/`KeyInit`
/// round-trip — NOT the 2400-byte expanded form. The secret key never leaves
/// the device, so the compact seed representation is what we persist.
pub const SECRETKEY_BYTES: usize = 64;
/// ML-KEM-768 ciphertext length (bytes).
pub const CIPHERTEXT_BYTES: usize = 1088;
/// Shared-secret length (bytes).
pub const SHAREDSECRET_BYTES: usize = 32;

// ---------------------------------------------------------------------------
//  Shared KDF/keystream (always compiled — used by the mesh garlic-wrapping
//  scaffold and by the offline KEM scaffold).
// ---------------------------------------------------------------------------

/// A small keyed byte-stream (splitmix64 over a 64-bit digest of `seed`).
/// Stands in for a PRF/XOF where a placeholder is acceptable (mesh layering).
fn kdf_fill(domain: u64, seed: &[u8], out: &mut [u8]) {
    let mut h: u64 = 0xcbf29ce484222325 ^ domain;
    for (i, &b) in seed.iter().enumerate() {
        h ^= (b as u64).wrapping_add(i as u64);
        h = h.wrapping_mul(0x100000001b3);
    }
    let mut ctr: u64 = 0;
    let mut i = 0;
    while i < out.len() {
        let mut z = h ^ ctr.wrapping_mul(0x9e3779b97f4a7c15);
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
        z ^= z >> 31;
        let w = z.to_le_bytes();
        let n = core::cmp::min(8, out.len() - i);
        out[i..i + n].copy_from_slice(&w[..n]);
        i += n;
        ctr += 1;
    }
}

/// Per-hop keystream pad used by the mesh garlic-wrapping scaffold.
/// (Domain-separated by `layer`.) Placeholder until per-hop ML-KEM keys land.
pub(crate) fn _layer_pad(layer: u64, out: &mut [u8]) {
    let seed = layer.to_le_bytes();
    kdf_fill(0x10 ^ layer, &seed, out);
}

// ---------------------------------------------------------------------------
//  Real ML-KEM-768 (default `pqc` feature)
// ---------------------------------------------------------------------------
#[cfg(feature = "pqc")]
mod pqc {
    use core::convert::Infallible;
    use ml_kem::{Decapsulate, Encapsulate, Kem, KeyExport, KeyInit, MlKem768, TryKeyInit};

    type Ek = <MlKem768 as Kem>::EncapsulationKey;
    type Dk = <MlKem768 as Kem>::DecapsulationKey;

    /// Adapts the kernel's core PRNG to `rand_core` (0.10) for ML-KEM. The
    /// fallible `TryRng`/`TryCryptoRng` traits are the primitives; `Rng` and
    /// `CryptoRng` are then blanket-derived for an `Infallible` error.
    struct CoreRng;
    impl rand_core::TryRng for CoreRng {
        type Error = Infallible;
        fn try_next_u32(&mut self) -> Result<u32, Infallible> {
            Ok(crate::rng_next() as u32)
        }
        fn try_next_u64(&mut self) -> Result<u64, Infallible> {
            Ok(crate::rng_next())
        }
        fn try_fill_bytes(&mut self, dst: &mut [u8]) -> Result<(), Infallible> {
            crate::rng_fill(dst);
            Ok(())
        }
    }
    impl rand_core::TryCryptoRng for CoreRng {}

    pub fn keypair(pk: &mut [u8], sk: &mut [u8]) {
        let mut rng = CoreRng;
        let (dk, ek) = MlKem768::generate_keypair_from_rng(&mut rng);
        pk.copy_from_slice(&ek.to_bytes()[..]); // KeyExport
        sk.copy_from_slice(&dk.to_bytes()[..]);
    }

    pub fn encapsulate(pk: &[u8], ct: &mut [u8], ss: &mut [u8]) {
        let mut rng = CoreRng;
        let ek = Ek::new_from_slice(pk).expect("ml-kem: bad public key"); // TryKeyInit
        let (ctv, ssv) = ek.encapsulate_with_rng(&mut rng);
        ct.copy_from_slice(&ctv[..]);
        ss.copy_from_slice(&ssv[..]);
    }

    pub fn decapsulate(sk: &[u8], ct: &[u8], ss: &mut [u8]) {
        let dk = Dk::new_from_slice(sk).expect("ml-kem: bad secret key"); // KeyInit
        let ssv = dk.decapsulate_slice(ct).expect("ml-kem: bad ciphertext");
        ss.copy_from_slice(&ssv[..]);
    }
}

#[cfg(feature = "pqc")]
pub use pqc::{decapsulate, encapsulate, keypair};

// ---------------------------------------------------------------------------
//  Offline scaffold (no default features) — NOT secure, builds without crates.
// ---------------------------------------------------------------------------
#[cfg(not(feature = "pqc"))]
mod scaffold {
    use super::*;

    const CORE_SECRET_BYTES: usize = 32;

    pub fn keypair(pk: &mut [u8], sk: &mut [u8]) {
        crate::rng_fill(sk);
        let mut core_secret = [0u8; CORE_SECRET_BYTES];
        core_secret.copy_from_slice(&sk[..CORE_SECRET_BYTES]);
        kdf_fill(0x01, &core_secret, pk);
    }

    pub fn encapsulate(pk: &[u8], ct: &mut [u8], ss: &mut [u8]) {
        let mut m = [0u8; SHAREDSECRET_BYTES];
        crate::rng_fill(&mut m);
        kdf_fill(0x02, &m, ss);
        let mut pad = [0u8; SHAREDSECRET_BYTES];
        kdf_fill(0x03, pk, &mut pad);
        for i in 0..SHAREDSECRET_BYTES {
            ct[i] = m[i] ^ pad[i];
        }
        kdf_fill(0x04, &m, &mut ct[SHAREDSECRET_BYTES..]);
    }

    pub fn decapsulate(sk: &[u8], ct: &[u8], ss: &mut [u8]) {
        let mut core_secret = [0u8; CORE_SECRET_BYTES];
        core_secret.copy_from_slice(&sk[..CORE_SECRET_BYTES]);
        let mut pk = [0u8; PUBLICKEY_BYTES];
        kdf_fill(0x01, &core_secret, &mut pk);
        let mut pad = [0u8; SHAREDSECRET_BYTES];
        kdf_fill(0x03, &pk, &mut pad);
        let mut m = [0u8; SHAREDSECRET_BYTES];
        for i in 0..SHAREDSECRET_BYTES {
            m[i] = ct[i] ^ pad[i];
        }
        kdf_fill(0x02, &m, ss);
    }
}

#[cfg(not(feature = "pqc"))]
pub use scaffold::{decapsulate, encapsulate, keypair};
