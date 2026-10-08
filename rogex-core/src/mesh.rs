//! Mesh / garlic-routing scaffold.
//!
//! Placeholders and fixed buffers so the C side has something to call.
//! No real multi-hop interop demonstrated. Don't cite this as a working mesh.


use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

/// Number of onion/garlic layers applied per outbound frame (hop count).
pub const GARLIC_LAYERS: usize = 3;
/// Maximum transmission unit for a mesh frame (bytes).
pub const MESH_MTU: usize = 1500;
/// Depth of the transmit ring.
const TX_RING_DEPTH: usize = 16;

struct Frame {
    len: usize,
    data: [u8; MESH_MTU],
}

struct TxRing {
    frames: [Frame; TX_RING_DEPTH],
    head: usize,
    tail: usize,
}

/// A minimal spinlock cell. The kernel is single-core in this scaffold; the
/// lock exists so the structure is `Sync` and ready for SMP later.
struct SpinCell<T> {
    locked: AtomicBool,
    inner: UnsafeCell<T>,
}
// SAFETY: access is serialized through `locked`.
unsafe impl<T> Sync for SpinCell<T> {}

impl<T> SpinCell<T> {
    const fn new(v: T) -> Self {
        SpinCell {
            locked: AtomicBool::new(false),
            inner: UnsafeCell::new(v),
        }
    }
    fn with<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
        while self
            .locked
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        // SAFETY: we hold the lock.
        let r = f(unsafe { &mut *self.inner.get() });
        self.locked.store(false, Ordering::Release);
        r
    }
}

static TX: SpinCell<TxRing> = SpinCell::new(TxRing {
    frames: [const {
        Frame {
            len: 0,
            data: [0u8; MESH_MTU],
        }
    }; TX_RING_DEPTH],
    head: 0,
    tail: 0,
});

static TX_ACCEPTED: AtomicUsize = AtomicUsize::new(0);
static TX_DROPPED: AtomicUsize = AtomicUsize::new(0);

/// Reset mesh state. Called from `rogex_core_init`.
pub fn init() {
    TX.with(|r| {
        r.head = 0;
        r.tail = 0;
    });
    TX_ACCEPTED.store(0, Ordering::Relaxed);
    TX_DROPPED.store(0, Ordering::Relaxed);
}

/// Apply the garlic layers to a frame in place.
///
/// Real implementation: for each hop, derive the hop key via ML-KEM
/// encapsulation to that node's public key (resolved through the DHT), then
/// wrap the payload in an AEAD layer with that key. Here we XOR-mask each
/// layer with a per-layer keystream so the transform is invertible and the
/// layering count is real, while being explicitly non-secure.
fn garlic_wrap(frame: &mut [u8]) {
    for layer in 0..GARLIC_LAYERS {
        let mut pad = [0u8; 32];
        // Domain-separate each hop's keystream by layer index.
        crate::crypto::_layer_pad(layer as u64, &mut pad);
        for (i, b) in frame.iter_mut().enumerate() {
            *b ^= pad[i % pad.len()];
        }
    }
}

/// Enqueue a datagram for transmission. Returns bytes accepted (0 if dropped).
pub fn tx(data: &[u8]) -> usize {
    let n = core::cmp::min(data.len(), MESH_MTU);
    TX.with(|r| {
        let next = (r.tail + 1) % TX_RING_DEPTH;
        if next == r.head {
            TX_DROPPED.fetch_add(1, Ordering::Relaxed);
            return 0; // ring full
        }
        let slot = &mut r.frames[r.tail];
        slot.data[..n].copy_from_slice(&data[..n]);
        slot.len = n;
        garlic_wrap(&mut slot.data[..n]);
        r.tail = next;
        TX_ACCEPTED.fetch_add(n, Ordering::Relaxed);
        n
    })
}

/// Total bytes accepted onto the TX path since init (diagnostics).
pub fn tx_accepted() -> usize {
    TX_ACCEPTED.load(Ordering::Relaxed)
}
