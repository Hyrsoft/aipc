use std::env;
fn main() {
    println!("cargo:rerun-if-changed=csrc/auxval_stub.c");

    let target = env::var("TARGET").expect("Cargo did not provide TARGET");
    if target.contains("uclibc") {
        cc::Build::new()
            .file("csrc/auxval_stub.c")
            .warnings(false)
            .compile("aipc_auxval_stub");
    }
}
