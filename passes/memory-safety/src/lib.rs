use nix::libc;
use std::collections::hash_map::Entry;
use std::collections::{BTreeMap, HashMap};
use std::ffi::c_void;
use std::ptr::{self, null_mut};
use std::sync::{Once, RwLock};

const REDZONE: usize = 16;

const SHADOW_SCALE: usize = 3;
const SHADOW_CHUNK_SHIFT: usize = 16;
const SHADOW_CHUNK_SIZE: usize = 1 << SHADOW_CHUNK_SHIFT;

#[derive(Copy, Clone)]
enum Kind {
    Heap,
    Stack,
}

#[derive(Copy, Clone)]
struct Meta {
    user: *mut u8,
    base: *mut u8,
    size: usize,
    kind: Kind,
}

struct State {
    meta_by_user: RwLock<HashMap<*mut u8, Meta>>,
    meta_by_start: RwLock<BTreeMap<usize, Meta>>,
    shadow: RwLock<HashMap<usize, *mut u8>>,
}

static INIT: Once = Once::new();
static mut STATE_PTR: *const State = std::ptr::null();

#[inline(always)]
fn shadow_index(addr: usize) -> usize {
    addr >> SHADOW_SCALE
}

#[inline(always)]
fn chunk_base_for_idx(idx: usize) -> usize {
    idx & !(SHADOW_CHUNK_SIZE - 1)
}

#[inline(always)]
fn offset_in_chunk(idx: usize) -> usize {
    idx & (SHADOW_CHUNK_SIZE - 1)
}

#[inline(always)]
unsafe fn get_or_alloc_chunk(
    map: &mut std::collections::HashMap<usize, *mut u8>,
    chunk_base: usize,
) -> *mut u8 {
    match map.entry(chunk_base) {
        Entry::Occupied(e) => *e.get(),
        Entry::Vacant(v) => {
            let p = libc::calloc(SHADOW_CHUNK_SIZE, 1) as *mut u8;

            if p.is_null() {
                libc::abort();
            }
            v.insert(p);
            p
        }
    }
}

fn state() -> &'static State {
    unsafe {
        INIT.call_once(|| {
            let st = State {
                meta_by_user: RwLock::new(HashMap::new()),
                meta_by_start: RwLock::new(BTreeMap::new()),
                shadow: RwLock::new(HashMap::new()),
            };
            STATE_PTR = Box::into_raw(Box::new(st));
        });
        &*STATE_PTR
    }
}

fn die() -> ! {
    let msg = b"Illegal memory access\n";
    unsafe {
        libc::write(
            libc::STDERR_FILENO,
            msg.as_ptr() as *const c_void,
            msg.len(),
        );
        libc::exit(-1);
    }
}

#[inline]
fn poison_range(addr: usize, size: usize, poison: bool) {
    if size == 0 {
        return;
    }

    let end = match addr.checked_add(size) {
        Some(v) => v,
        None => return,
    };

    let start_aligned = addr & !0x7;
    let end_aligned = (end + 7) & !0x7;

    let start_idx = shadow_index(start_aligned);
    let end_idx = shadow_index(end_aligned);

    if start_idx >= end_idx {
        return;
    }

    let st = state();
    let fill: u8 = if poison { 0xFF } else { 0x00 };

    let mut sh = st.shadow.write().unwrap();

    let mut idx = start_idx;
    while idx < end_idx {
        let chunk_base = chunk_base_for_idx(idx);
        let chunk_off = offset_in_chunk(idx);

        let chunk_end_idx = (chunk_base + SHADOW_CHUNK_SIZE).min(end_idx);
        let chunk_end_off = offset_in_chunk(chunk_end_idx);

        let chunk_ptr = unsafe { get_or_alloc_chunk(&mut sh, chunk_base) };

        let len = if chunk_end_off == 0 && chunk_end_idx > chunk_base {
            SHADOW_CHUNK_SIZE - chunk_off
        } else {
            chunk_end_off.saturating_sub(chunk_off)
        };

        unsafe {
            ptr::write_bytes(chunk_ptr.add(chunk_off), fill, len);
        }

        idx = chunk_end_idx;
    }
}

#[inline]
fn shadow_is_poisoned(addr: usize, size: usize) -> bool {
    if size == 0 {
        return false;
    }

    let end = match addr.checked_add(size) {
        Some(v) => v,
        None => return true,
    };

    let start_idx = shadow_index(addr);
    let end_idx = shadow_index(end + 7);

    if start_idx >= end_idx {
        return false;
    }

    let st = state();
    let sh = st.shadow.read().unwrap();

    let mut idx = start_idx;
    while idx < end_idx {
        let chunk_base = chunk_base_for_idx(idx);
        let chunk_off = offset_in_chunk(idx);

        let chunk_end_idx = (chunk_base + SHADOW_CHUNK_SIZE).min(end_idx);
        let chunk_end_off = offset_in_chunk(chunk_end_idx);

        if let Some(&chunk_ptr) = sh.get(&chunk_base) {
            let len = if chunk_end_off == 0 && chunk_end_idx > chunk_base {
                SHADOW_CHUNK_SIZE - chunk_off
            } else {
                chunk_end_off.saturating_sub(chunk_off)
            };

            let slice = unsafe { std::slice::from_raw_parts(chunk_ptr.add(chunk_off), len) };
            if slice.iter().any(|&b| b != 0) {
                return true;
            }
        }

        idx = chunk_end_idx;
    }

    false
}

#[no_mangle]
pub extern "C" fn __runtime_init() {
    let _ = state();
}

#[no_mangle]
pub extern "C" fn __runtime_cleanup() {
    let st = state();

    {
        let mut by_user = st.meta_by_user.write().unwrap();

        for (_user_ptr, m) in by_user.drain() {
            match m.kind {
                Kind::Heap => unsafe {
                    libc::free(m.base as *mut c_void);
                },
                Kind::Stack => {}
            }
        }
    }

    {
        st.meta_by_start.write().unwrap().clear();
    }

    {
        let mut sh = st.shadow.write().unwrap();
        for (_base, ptr) in sh.drain() {
            unsafe { libc::free(ptr as *mut c_void) };
        }
    }
}

#[no_mangle]
pub extern "C" fn __runtime_check_addr(addr: *const c_void, size: usize) {
    if addr.is_null() || size == 0 {
        return;
    }

    let a = addr as usize;
    let end = a.checked_add(size).unwrap_or_else(|| die());

    let st = state();

    if let Some((_start, m)) = st.meta_by_start.read().unwrap().range(..=a).next_back() {
        let start = m.user as usize;
        let end_m = start + m.size;

        if a >= start && end <= end_m {
            if shadow_is_poisoned(a, size) {
                die();
            }
            return;
        }
    }

    if shadow_is_poisoned(a, size) {
        die();
    }
}

#[no_mangle]
pub extern "C" fn __runtime_malloc(size: usize) -> *mut c_void {
    let total = match size.checked_add(2 * REDZONE) {
        Some(v) => v,
        None => return null_mut(),
    };

    let base = unsafe { libc::malloc(total) as *mut u8 };
    if base.is_null() {
        return null_mut();
    }

    let user = unsafe { base.add(REDZONE) };

    poison_range(base as usize, REDZONE, true);
    poison_range(user as usize, size, false);
    poison_range((user as usize).saturating_add(size), REDZONE, true);

    let meta_rec = Meta {
        user,
        base,
        size,
        kind: Kind::Heap,
    };

    let st = state();
    st.meta_by_user.write().unwrap().insert(user, meta_rec);
    st.meta_by_start
        .write()
        .unwrap()
        .insert(user as usize, meta_rec);

    user as *mut c_void
}

#[no_mangle]
pub extern "C" fn __runtime_stack_alloc(ptr: *mut c_void, size: usize) {
    if ptr.is_null() || size == 0 {
        return;
    }

    let user = ptr as usize;
    let base = user - REDZONE;

    poison_range(base, REDZONE, true);

    poison_range(user, size, false);

    poison_range(user + size, REDZONE, true);

    let meta_rec = Meta {
        user: ptr as *mut u8,
        base: null_mut(),
        size,
        kind: Kind::Stack,
    };

    let st = state();
    st.meta_by_user
        .write()
        .unwrap()
        .insert(ptr as *mut u8, meta_rec);
    st.meta_by_start.write().unwrap().insert(user, meta_rec);
}

#[no_mangle]
pub extern "C" fn __runtime_free(ptr: *mut c_void) {
    if ptr.is_null() {
        return;
    }

    let user = ptr as *mut u8;

    let st = state();

    let mut by_user = st.meta_by_user.write().unwrap();
    let m = match by_user.remove(&user) {
        Some(v) => v,
        None => die(),
    };

    st.meta_by_start.write().unwrap().remove(&(user as usize));

    poison_range(user as usize, m.size, true);

    match m.kind {
        Kind::Heap => {
            poison_range(m.base as usize, REDZONE, true);
            poison_range((user as usize).saturating_add(m.size), REDZONE, true);

            unsafe {
                libc::free(m.base as *mut c_void);
            }
        }
        Kind::Stack => {
            die();
        }
    }
}

#[no_mangle]
pub extern "C" fn __runtime_stack_unpoison(ptr: *mut c_void, size: usize) {
    if ptr.is_null() || size == 0 {
        return;
    }
    poison_range(ptr as usize, size, false);
}

#[no_mangle]
pub extern "C" fn __runtime_stack_poison(ptr: *mut c_void, size: usize) {
    if ptr.is_null() || size == 0 {
        return;
    }
    poison_range(ptr as usize, size, true);
}
