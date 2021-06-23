#![allow(non_snake_case)]
#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int};
use std::sync::Arc;

use crate::bytestring::ByteString;
use crate::cffi::{self, Allocator_t, Random_t, String_t};
use crate::crypto::crypto_auth;
use crate::crypto::keys::{PrivateKey, PublicKey};
use crate::external::interface::cif;
use crate::external::memory::allocator;
use crate::rtypes::*;

// This file is used to generate cbindings.h using cbindgen

#[no_mangle]
pub unsafe extern "C" fn Rffi_testwrapper_create(a: *mut Allocator_t) -> RTypes_IfWrapper_t {
    let w = crate::interface::rustiface_test_wrapper::TestWrapper::default();
    RTypes_IfWrapper_t {
        external: cif::wrap(a, w.ext),
        internal: cif::wrap(a, w.int),
    }
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_android_create(a: *mut Allocator_t) -> RTypes_IfWrapper_t {
    let w = crate::interface::tuntap::android::AndroidWrapper::default();
    RTypes_IfWrapper_t {
        external: cif::wrap(a, w.ext),
        internal: cif::wrap(a, w.int),
    }
}

unsafe fn cu8(s: *const u8, len: usize) -> Vec<u8> {
    std::slice::from_raw_parts::<u8>(s, len).to_vec()
}

unsafe fn cstr(s: *const String_t) -> Option<ByteString> {
    if s.is_null() {
        None
    } else {
        let bytes = cu8((*s).bytes as *const u8, (*s).len);
        Some(ByteString(bytes))
    }
}

unsafe fn strc(alloc: *mut Allocator_t, s: ByteString) -> *mut String_t {
    let ByteString(mut s) = s;
    let len = s.len();
    let bytes = s.as_mut_ptr() as *mut i8;
    allocator::adopt(alloc, s);
    allocator::adopt(alloc, String_t { len, bytes })
}

#[repr(C)]
pub struct Rffi_CryptoAuth2_Session_t {
    r: RTypes_CryptoAuth2_Session_t,
    s: Arc<crypto_auth::Session>,
}

pub struct RTypes_CryptoAuth2_t(pub Arc<crypto_auth::CryptoAuth>);

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_addUser_ipv6(
    password: *mut String_t,
    login: *mut String_t,
    ipv6: *mut u8,
    ca: *mut RTypes_CryptoAuth2_t,
) -> c_int {
    let ip6 = if ipv6.is_null() {
        None
    } else {
        let mut ip = [0_u8; 16];
        ip.copy_from_slice(std::slice::from_raw_parts(ipv6, 16));
        Some(ip)
    };
    match (*ca)
        .0
        .add_user_ipv6(cstr(password).expect("password"), cstr(login), ip6)
    {
        Ok(_) => 0,
        Err(crypto_auth::AddUserError::Duplicate { .. }) => {
            cffi::CryptoAuth_addUser_Res::CryptoAuth_addUser_DUPLICATE as i32
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_removeUsers(
    context: *mut RTypes_CryptoAuth2_t,
    user: *mut String_t,
) -> c_int {
    (*context).0.remove_users(cstr(user)) as c_int
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_getUsers(
    ca: *const RTypes_CryptoAuth2_t,
    alloc: *mut Allocator_t,
) -> *mut RTypes_StrList_t {
    let mut users = (*ca).0.get_users();
    let mut str_users = users.drain(..).map(|u| strc(alloc, u)).collect::<Vec<_>>();
    let out = RTypes_StrList_t {
        len: str_users.len(),
        items: str_users.as_mut_ptr(),
    };
    allocator::adopt(alloc, str_users);
    allocator::adopt(alloc, out)
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_new(
    allocator: *mut Allocator_t,
    privateKey: *const u8,
    random: *mut Random_t,
) -> *mut RTypes_CryptoAuth2_t {
    allocator::adopt(
        allocator,
        RTypes_CryptoAuth2_t(Arc::new(crypto_auth::CryptoAuth::new(
            if privateKey.is_null() {
                None
            } else {
                let mut bytes = [0_u8; 32];
                bytes.copy_from_slice(std::slice::from_raw_parts(privateKey, 32));
                Some(PrivateKey::from(bytes))
            },
            crate::util::events::EventBase {},
            //crate::crypto::random::Random::new_sodium().expect("libsodium init() failed"),
            crate::crypto::random::Random::wrap_legacy(random),
        ))),
    )
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_newSession(
    ca: *mut RTypes_CryptoAuth2_t,
    alloc: *mut Allocator_t,
    herPublicKey: *const u8,
    requireAuth: bool,
    name: *const c_char,
    useNoise: bool,
) -> *mut RTypes_CryptoAuth2_Session_t {
    let (session, plaintext, ciphertext) = crypto_auth::Session::new(
        Arc::clone(&(*ca).0),
        {
            let mut bytes = [0_u8; 32];
            bytes.copy_from_slice(std::slice::from_raw_parts(herPublicKey, 32));
            PublicKey::from(bytes)
        },
        requireAuth,
        if name.is_null() {
            None
        } else {
            match std::ffi::CStr::from_ptr(name).to_str() {
                Ok(s) => Some(s.to_string()),
                Err(_) => None,
            }
        },
        useNoise,
    )
    .expect("bad public key"); //TODO Pass the error to C code somehow instead of `expect()`.
    &mut allocator::adopt(
        alloc,
        Rffi_CryptoAuth2_Session_t {
            r: RTypes_CryptoAuth2_Session_t {
                ciphertext: cif::wrap(alloc, ciphertext),
                plaintext: cif::wrap(alloc, plaintext),
            },
            s: session,
        },
    )
    .r as *mut _
}

unsafe fn ffi_sess_mut(
    s: *mut RTypes_CryptoAuth2_Session_t,
) -> &'static mut Rffi_CryptoAuth2_Session_t {
    &mut *(s as *mut Rffi_CryptoAuth2_Session_t)
}
unsafe fn ffi_sess(s: *const RTypes_CryptoAuth2_Session_t) -> &'static Rffi_CryptoAuth2_Session_t {
    &*(s as *const Rffi_CryptoAuth2_Session_t)
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_setAuth(
    password: *const String_t,
    login: *const String_t,
    caSession: *mut RTypes_CryptoAuth2_Session_t,
) {
    ffi_sess_mut(caSession)
        .s
        .set_auth(cstr(password), cstr(login))
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_resetIfTimeout(
    session: *mut RTypes_CryptoAuth2_Session_t,
) {
    ffi_sess_mut(session).s.reset_if_timeout()
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_reset(caSession: *mut RTypes_CryptoAuth2_Session_t) {
    ffi_sess_mut(caSession).s.reset()
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_getState(
    session: *mut RTypes_CryptoAuth2_Session_t,
) -> RTypes_CryptoAuth_State_t {
    ffi_sess_mut(session).s.get_state()
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_getHerPubKey(
    session: *const RTypes_CryptoAuth2_Session_t,
    pkOut: *mut u8,
) {
    let p = ffi_sess(session).s.get_her_pubkey();
    std::slice::from_raw_parts_mut(pkOut, 32).copy_from_slice(&p[..]);
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_getHerIp6(
    session: *const RTypes_CryptoAuth2_Session_t,
    ipOut: *mut u8,
) {
    let p = ffi_sess(session).s.get_her_ip6();
    std::slice::from_raw_parts_mut(ipOut, 16).copy_from_slice(&p[..]);
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_getName(
    session: *const RTypes_CryptoAuth2_Session_t,
    alloc: *mut Allocator_t,
) -> *mut String_t {
    match ffi_sess(session).s.get_name() {
        None => std::ptr::null_mut::<String_t>(),
        Some(name) => strc(alloc, ByteString::from(name)),
    }
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_getPubKey(
    ca: *const RTypes_CryptoAuth2_t,
    pkOut: *mut u8,
) {
    let p = (*ca).0.public_key.raw();
    std::slice::from_raw_parts_mut(pkOut, 32).copy_from_slice(&p[..]);
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_CryptoAuth2_stats(
    session: *const RTypes_CryptoAuth2_Session_t,
    statsOut: *mut RTypes_CryptoStats_t,
) {
    let st = ffi_sess(session).s.stats();
    (*statsOut) = st;
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_panic(msg: *const c_char) -> ! {
    panic!("{}", std::ffi::CStr::from_ptr(msg).to_string_lossy())
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_setLogger(l: *mut cffi::Log_t) {
    crate::cjdnslog::set_ffi_logger(l);
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_error(
    msg: *const c_char,
    alloc: *mut Allocator_t,
) -> *mut RTypes_Error_t {
    let s = std::ffi::CStr::from_ptr(msg).to_string_lossy();
    allocator::adopt(alloc, RTypes_Error_t { e: Some(anyhow::anyhow!(s)) })
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_error_fl(
    msg: *const c_char,
    file: *const c_char,
    line: c_int,
    alloc: *mut Allocator_t,
) -> *mut RTypes_Error_t {
    let s = std::ffi::CStr::from_ptr(msg).to_string_lossy();
    let f = std::ffi::CStr::from_ptr(file).to_string_lossy();
    let ss = format!("{}:{}: {}", f, line, s);
    allocator::adopt(alloc, RTypes_Error_t { e: Some(anyhow::anyhow!(ss)) })
}

fn str_to_c(s: &str, alloc: *mut Allocator_t) -> *const c_char {
    let c_str = std::ffi::CString::new(s).unwrap();
    let adopted = allocator::adopt(alloc, c_str);
    return (*adopted).as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn Rffi_printError(
    e: *mut RTypes_Error_t,
    alloc: *mut Allocator_t,
) -> *const c_char {
    e.as_ref()
        .map(|e|e.e.as_ref())
        .flatten()
        .map(|e|str_to_c(&format!("{:?}", e), alloc))
        .unwrap_or_else(std::ptr::null)
}