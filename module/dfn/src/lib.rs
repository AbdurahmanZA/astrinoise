//! astrinoise-dfn: the DeepFilterNet3 engine for Astri-noise, as a small C API.
//!
//! func_astrinoise.so loads this library with dlopen() when it is installed. Each cleaned call
//! direction gets its own state and calls `astrinoise_dfn_process()` once per 10 ms frame, in the
//! channel's own audio thread, like RNNoise. Nothing here starts a thread or keeps a queue.
//!
//! We do not use DeepFilterNet's own LADSPA plugin: each plugin instance starts a worker thread that
//! polls every 2 ms and never exits, its delay grows after a slow frame, and its instances are
//! clones that share two feature buffers. Its own C API panics on errors, and a Rust panic that
//! reaches Asterisk stops the whole PBX. So every function here catches panics and returns an
//! error instead.
//!
//! One base model is built once. Each state is a `fresh_copy()` of it (patches/libdf-fresh-copy.patch):
//! the read-only weights are shared, so 30 calls read one copy of them instead of 30, and each
//! state has its own buffers. One state must only be used by one thread at a time.
//! Lab switch: ASTRINOISE_DFN_SHARED=0 in Asterisk's environment builds every state on its own.
//!
//! Copyright (C) 2026, SANDZ (Solomon and Zoutenburg (Pty) Ltd). MIT OR Apache-2.0, the same terms
//! as DeepFilterNet, because this library links DeepFilterNet, tract and prost (Apache-2.0) into one
//! file. It is separate from func_astrinoise.so (GPL-2.0), which opens it at run time if installed.
//! DeepFilterNet and its DFN3 model: Hendrik Schröter, MIT OR Apache-2.0.

use std::ffi::{c_char, c_float, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::Mutex;

use df::tract::{DfParams, DfTract, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};

/// Bump when a function signature changes; func_astrinoise refuses other versions.
const ABI: c_int = 1;

/// The base model. It never processes audio; it is only copied, under this mutex.
struct Base(DfTract);
// SAFETY: DfTract is not Send because it holds Rc-counted buffers. The base is only touched while
// the mutex is held, and fresh_copy() gives every copy its own buffers, so no Rc is shared between
// threads. The weights the copies share are Arc-counted.
unsafe impl Send for Base {}
static BASE: Mutex<Option<Base>> = Mutex::new(None);

pub struct Dfn {
    df: DfTract,
}

fn build() -> Option<DfTract> {
    // Unpacks the built-in DFN3 model (tar.gz) and optimises it: about 0.1 s.
    // Same stage thresholds as DeepFilterNet's own deep-filter tool (min -15, ERB 35, DF 35 dB).
    // The library defaults (-10, 30, 20) scored PESQ-WB 2.02 against the tool's 2.90 in our test.
    let rp = RuntimeParams::default_with_ch(1).with_atten_lim(100.).with_thresholds(-15., 35., 35.);
    DfTract::new(DfParams::default(), &rp).ok()
}

fn new_state() -> Option<DfTract> {
    if std::env::var("ASTRINOISE_DFN_SHARED").map(|v| v == "0").unwrap_or(false) {
        return build();
    }
    let mut b = BASE.lock().ok()?;
    if b.is_none() {
        *b = Some(Base(build()?));
    }
    b.as_ref()?.0.fresh_copy().ok()
}

#[no_mangle]
pub extern "C" fn astrinoise_dfn_abi() -> c_int {
    ABI
}

/// Version text, for `astrinoise show stats`.
#[no_mangle]
pub extern "C" fn astrinoise_dfn_version() -> *const c_char {
    concat!("astrinoise-dfn ", env!("CARGO_PKG_VERSION"), " (DeepFilterNet 0.5.6, DFN3)\0").as_ptr() as *const c_char
}

/// A new state for one audio direction, or NULL. The first one builds the base model (about
/// 0.1 s), later ones copy it. func_astrinoise calls this from its pool thread or the dialplan,
/// never per frame. `atten_lim_db`: most noise reduction in dB, 100 = no limit.
#[no_mangle]
pub extern "C" fn astrinoise_dfn_new(atten_lim_db: c_float) -> *mut Dfn {
    let r = catch_unwind(|| {
        let mut df = new_state()?;
        df.set_atten_lim(atten_lim_db);
        Some(df)
    });
    match r {
        Ok(Some(df)) => Box::into_raw(Box::new(Dfn { df })),
        _ => std::ptr::null_mut(),
    }
}

/// Samples per frame at 48 kHz (480 = 10 ms for DFN3), or 0 on error.
#[no_mangle]
pub extern "C" fn astrinoise_dfn_frame_len(st: *const Dfn) -> c_int {
    if st.is_null() {
        return 0;
    }
    let st = unsafe { &*st };
    st.df.hop_size as c_int
}

/// Most noise reduction in dB (100 = no limit).
#[no_mangle]
pub extern "C" fn astrinoise_dfn_set_atten_lim(st: *mut Dfn, db: c_float) {
    if st.is_null() {
        return;
    }
    let st = unsafe { &mut *st };
    let _ = catch_unwind(AssertUnwindSafe(|| st.df.set_atten_lim(db)));
}

/// Clean one frame of `astrinoise_dfn_frame_len()` samples, 48 kHz, scaled to -1..1.
/// Returns 0, or -1 if DeepFilterNet failed (then `output` holds a copy of `input`).
#[no_mangle]
pub extern "C" fn astrinoise_dfn_process(st: *mut Dfn, input: *const c_float, output: *mut c_float) -> c_int {
    if st.is_null() || input.is_null() || output.is_null() {
        return -1;
    }
    let st = unsafe { &mut *st };
    let n = st.df.hop_size;
    let r = catch_unwind(AssertUnwindSafe(|| {
        let i = unsafe { ArrayView2::from_shape_ptr((1, n), input) };
        let o = unsafe { ArrayViewMut2::from_shape_ptr((1, n), output) };
        st.df.process(i, o).is_ok()
    }));
    if matches!(r, Ok(true)) {
        0
    } else {
        unsafe { std::ptr::copy(input, output, n) };
        -1
    }
}

#[no_mangle]
pub extern "C" fn astrinoise_dfn_free(st: *mut Dfn) {
    if !st.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(st) })));
    }
}
