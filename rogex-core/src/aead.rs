//! ChaCha20-Poly1305 (RFC 8439), hand-rolled for bare metal.
//!
//! In-tree pure integer math: the usual crates pull CPU-feature dispatch that
//! fights `-C code-model=kernel`. Checked against RFC 8439 §2.8.2 at boot.
//! Useful; not audited. Don't bet the farm on it alone.


#[inline]
fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

// ---------------------------------- ChaCha20 --------------------------------
#[inline]
fn qr(s: &mut [u32; 16], a: usize, b: usize, c: usize, d: usize) {
    s[a] = s[a].wrapping_add(s[b]); s[d] ^= s[a]; s[d] = s[d].rotate_left(16);
    s[c] = s[c].wrapping_add(s[d]); s[b] ^= s[c]; s[b] = s[b].rotate_left(12);
    s[a] = s[a].wrapping_add(s[b]); s[d] ^= s[a]; s[d] = s[d].rotate_left(8);
    s[c] = s[c].wrapping_add(s[d]); s[b] ^= s[c]; s[b] = s[b].rotate_left(7);
}

fn chacha_block(key: &[u8; 32], counter: u32, nonce: &[u8; 12], out: &mut [u8; 64]) {
    let mut s = [0u32; 16];
    s[0] = 0x6170_7865; s[1] = 0x3320_646e; s[2] = 0x7962_2d32; s[3] = 0x6b20_6574;
    for i in 0..8 {
        s[4 + i] = le32(&key[i * 4..]);
    }
    s[12] = counter;
    for i in 0..3 {
        s[13 + i] = le32(&nonce[i * 4..]);
    }
    let mut w = s;
    for _ in 0..10 {
        qr(&mut w, 0, 4, 8, 12); qr(&mut w, 1, 5, 9, 13);
        qr(&mut w, 2, 6, 10, 14); qr(&mut w, 3, 7, 11, 15);
        qr(&mut w, 0, 5, 10, 15); qr(&mut w, 1, 6, 11, 12);
        qr(&mut w, 2, 7, 8, 13); qr(&mut w, 3, 4, 9, 14);
    }
    for i in 0..16 {
        let v = w[i].wrapping_add(s[i]);
        out[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
    }
}

/// XOR `data` with the ChaCha20 keystream, block counter starting at `counter`.
fn chacha_xor(key: &[u8; 32], nonce: &[u8; 12], mut counter: u32, data: &mut [u8]) {
    let mut ks = [0u8; 64];
    let mut off = 0;
    while off < data.len() {
        chacha_block(key, counter, nonce, &mut ks);
        let n = core::cmp::min(64, data.len() - off);
        for i in 0..n {
            data[off + i] ^= ks[i];
        }
        off += n;
        counter = counter.wrapping_add(1);
    }
}

// ---------------------------------- Poly1305 --------------------------------
// 5 x 26-bit limb ("donna32") accumulator.
struct Poly {
    r: [u32; 5],
    h: [u32; 5],
    pad: [u32; 4],
}

impl Poly {
    fn new(key: &[u8; 32]) -> Poly {
        let t0 = le32(&key[0..]); let t1 = le32(&key[4..]);
        let t2 = le32(&key[8..]); let t3 = le32(&key[12..]);
        Poly {
            r: [
                t0 & 0x3ff_ffff,
                ((t0 >> 26) | (t1 << 6)) & 0x3ff_ff03,
                ((t1 >> 20) | (t2 << 12)) & 0x3ff_c0ff,
                ((t2 >> 14) | (t3 << 18)) & 0x3f0_3fff,
                (t3 >> 8) & 0x00f_ffff,
            ],
            h: [0; 5],
            pad: [le32(&key[16..]), le32(&key[20..]), le32(&key[24..]), le32(&key[28..])],
        }
    }

    fn block(&mut self, m: &[u8; 16]) {
        let t0 = le32(&m[0..]); let t1 = le32(&m[4..]);
        let t2 = le32(&m[8..]); let t3 = le32(&m[12..]);
        self.h[0] = self.h[0].wrapping_add(t0 & 0x3ff_ffff);
        self.h[1] = self.h[1].wrapping_add(((t0 >> 26) | (t1 << 6)) & 0x3ff_ffff);
        self.h[2] = self.h[2].wrapping_add(((t1 >> 20) | (t2 << 12)) & 0x3ff_ffff);
        self.h[3] = self.h[3].wrapping_add(((t2 >> 14) | (t3 << 18)) & 0x3ff_ffff);
        self.h[4] = self.h[4].wrapping_add((t3 >> 8) | (1 << 24)); // append the 2^128 bit

        let r0 = self.r[0] as u64; let r1 = self.r[1] as u64; let r2 = self.r[2] as u64;
        let r3 = self.r[3] as u64; let r4 = self.r[4] as u64;
        let (s1, s2, s3, s4) = (r1 * 5, r2 * 5, r3 * 5, r4 * 5);
        let h0 = self.h[0] as u64; let h1 = self.h[1] as u64; let h2 = self.h[2] as u64;
        let h3 = self.h[3] as u64; let h4 = self.h[4] as u64;

        let d0 = h0 * r0 + h1 * s4 + h2 * s3 + h3 * s2 + h4 * s1;
        let mut d1 = h0 * r1 + h1 * r0 + h2 * s4 + h3 * s3 + h4 * s2;
        let mut d2 = h0 * r2 + h1 * r1 + h2 * r0 + h3 * s4 + h4 * s3;
        let mut d3 = h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * s4;
        let mut d4 = h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;

        let mut c;
        c = d0 >> 26; let mut n0 = (d0 as u32) & 0x3ff_ffff; d1 += c;
        c = d1 >> 26; let n1 = (d1 as u32) & 0x3ff_ffff; d2 += c;
        c = d2 >> 26; let n2 = (d2 as u32) & 0x3ff_ffff; d3 += c;
        c = d3 >> 26; let n3 = (d3 as u32) & 0x3ff_ffff; d4 += c;
        c = d4 >> 26; let n4 = (d4 as u32) & 0x3ff_ffff; n0 = n0.wrapping_add((c as u32) * 5);
        let cc = n0 >> 26; n0 &= 0x3ff_ffff; let n1 = n1.wrapping_add(cc);
        self.h = [n0, n1, n2, n3, n4];
    }

    /// Feed a data section, zero-padding a trailing partial block (RFC pad16).
    fn update(&mut self, data: &[u8]) {
        let mut i = 0;
        while i + 16 <= data.len() {
            let mut b = [0u8; 16];
            b.copy_from_slice(&data[i..i + 16]);
            self.block(&b);
            i += 16;
        }
        if i < data.len() {
            let mut b = [0u8; 16];
            b[..data.len() - i].copy_from_slice(&data[i..]);
            self.block(&b);
        }
    }

    fn finalize(mut self, out: &mut [u8; 16]) {
        let mut c;
        c = self.h[1] >> 26; self.h[1] &= 0x3ff_ffff; self.h[2] += c;
        c = self.h[2] >> 26; self.h[2] &= 0x3ff_ffff; self.h[3] += c;
        c = self.h[3] >> 26; self.h[3] &= 0x3ff_ffff; self.h[4] += c;
        c = self.h[4] >> 26; self.h[4] &= 0x3ff_ffff; self.h[0] += c * 5;
        c = self.h[0] >> 26; self.h[0] &= 0x3ff_ffff; self.h[1] += c;

        let mut g0 = self.h[0].wrapping_add(5); c = g0 >> 26; g0 &= 0x3ff_ffff;
        let mut g1 = self.h[1].wrapping_add(c); c = g1 >> 26; g1 &= 0x3ff_ffff;
        let mut g2 = self.h[2].wrapping_add(c); c = g2 >> 26; g2 &= 0x3ff_ffff;
        let mut g3 = self.h[3].wrapping_add(c); c = g3 >> 26; g3 &= 0x3ff_ffff;
        let g4 = self.h[4].wrapping_add(c).wrapping_sub(1 << 26);

        let mask = (g4 >> 31).wrapping_sub(1); // g4 underflowed => keep h, else use g
        g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask;
        let g4m = g4 & mask;
        let nmask = !mask;
        self.h[0] = (self.h[0] & nmask) | g0;
        self.h[1] = (self.h[1] & nmask) | g1;
        self.h[2] = (self.h[2] & nmask) | g2;
        self.h[3] = (self.h[3] & nmask) | g3;
        self.h[4] = (self.h[4] & nmask) | g4m;

        let h0 = (self.h[0] | (self.h[1] << 26)) as u64 & 0xffff_ffff;
        let h1 = ((self.h[1] >> 6) | (self.h[2] << 20)) as u64 & 0xffff_ffff;
        let h2 = ((self.h[2] >> 12) | (self.h[3] << 14)) as u64 & 0xffff_ffff;
        let h3 = ((self.h[3] >> 18) | (self.h[4] << 8)) as u64 & 0xffff_ffff;

        let mut f = h0 + self.pad[0] as u64; let o0 = f as u32;
        f = h1 + self.pad[1] as u64 + (f >> 32); let o1 = f as u32;
        f = h2 + self.pad[2] as u64 + (f >> 32); let o2 = f as u32;
        f = h3 + self.pad[3] as u64 + (f >> 32); let o3 = f as u32;
        out[0..4].copy_from_slice(&o0.to_le_bytes());
        out[4..8].copy_from_slice(&o1.to_le_bytes());
        out[8..12].copy_from_slice(&o2.to_le_bytes());
        out[12..16].copy_from_slice(&o3.to_le_bytes());
    }
}

fn poly_tag(key: &[u8; 32], nonce: &[u8; 12], aad: &[u8], ct: &[u8], tag: &mut [u8; 16]) {
    let mut block0 = [0u8; 64];
    chacha_block(key, 0, nonce, &mut block0); // block 0 -> one-time Poly1305 key
    let mut pk = [0u8; 32];
    pk.copy_from_slice(&block0[..32]);
    let mut poly = Poly::new(&pk);
    poly.update(aad);
    poly.update(ct);
    let mut lb = [0u8; 16];
    lb[0..8].copy_from_slice(&(aad.len() as u64).to_le_bytes());
    lb[8..16].copy_from_slice(&(ct.len() as u64).to_le_bytes());
    poly.block(&lb);
    poly.finalize(tag);
}

/// Constant-time 16-byte tag comparison.
fn ct_eq16(a: &[u8; 16], b: &[u8]) -> bool {
    if b.len() != 16 {
        return false;
    }
    let mut acc = 0u8;
    for i in 0..16 {
        acc |= a[i] ^ b[i];
    }
    acc == 0
}

/// Seal `buf` in place (plaintext -> ciphertext); write the 16-byte tag.
pub fn seal(key: &[u8; 32], nonce: &[u8; 12], aad: &[u8], buf: &mut [u8], tag: &mut [u8; 16]) {
    chacha_xor(key, nonce, 1, buf); // payload keystream starts at counter 1
    poly_tag(key, nonce, aad, buf, tag);
}

/// Verify the tag over `buf` (ciphertext) and, if valid, decrypt in place.
/// Returns `true` on success; on failure `buf` is left untouched.
pub fn open(key: &[u8; 32], nonce: &[u8; 12], aad: &[u8], buf: &mut [u8], tag: &[u8]) -> bool {
    let mut expected = [0u8; 16];
    poly_tag(key, nonce, aad, buf, &mut expected);
    if !ct_eq16(&expected, tag) {
        return false;
    }
    chacha_xor(key, nonce, 1, buf);
    true
}

/// RFC 8439 §2.8.2 known-answer test. Returns `true` if the implementation is
/// correct. Runs at boot so a miscompiled crypto core fails loudly.
pub fn selftest() -> bool {
    let mut key = [0u8; 32];
    for i in 0..32 {
        key[i] = 0x80 + i as u8;
    }
    let nonce = [0x07, 0, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47];
    let aad = [0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7];
    let pt = b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    let mut buf = [0u8; 114];
    buf.copy_from_slice(pt);
    let mut tag = [0u8; 16];
    seal(&key, &nonce, &aad, &mut buf, &mut tag);

    let exp_tag = [
        0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
        0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91,
    ];
    let exp_ct0 = [0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb];
    if !ct_eq16(&tag, &exp_tag) || buf[0..8] != exp_ct0 {
        return false;
    }
    // Round-trip: open must recover the plaintext and accept the tag.
    if !open(&key, &nonce, &aad, &mut buf, &tag) {
        return false;
    }
    buf[..] == pt[..]
}
