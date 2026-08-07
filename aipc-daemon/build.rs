use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=AIPC_SDK_ROOT");
    println!("cargo:rerun-if-env-changed=AIPC_BUILD_MEDIA_WORKER");
    println!("cargo:rerun-if-changed=../media_worker/CMakeLists.txt");
    println!("cargo:rerun-if-changed=../media_worker/src");
    println!("cargo:rerun-if-changed=../media_worker/config");
    println!("cargo:rerun-if-changed=../ai_worker/CMakeLists.txt");
    println!("cargo:rerun-if-changed=../ai_worker/src");
    println!("cargo:rerun-if-changed=../3rdparty/nlohmann_json/include");
    println!("cargo:rerun-if-changed=csrc/auxval_stub.c");

    let target = env::var("TARGET").expect("Cargo did not provide TARGET");
    if target.contains("uclibc") {
        cc::Build::new()
            .file("csrc/auxval_stub.c")
            .warnings(false)
            .compile("aipc_auxval_stub");
    }

    let force_worker = env::var("AIPC_BUILD_MEDIA_WORKER").as_deref() == Ok("1");
    if target != "armv7-unknown-linux-uclibceabihf" && !force_worker {
        println!("cargo:warning=host build: skipping RV1106 media_worker CMake build");
        return;
    }

    build_media_worker(&target);
}

fn build_media_worker(target: &str) {
    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let workspace_root = manifest_dir.parent().unwrap().to_path_buf();
    let sdk_root = env::var_os("AIPC_SDK_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| workspace_root.parent().unwrap().to_path_buf());
    let toolchain_dir =
        sdk_root.join("tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf");
    let prefix = toolchain_dir.join("bin/arm-rockchip830-linux-uclibcgnueabihf");
    require_executable(&prefix.with_file_name("arm-rockchip830-linux-uclibcgnueabihf-gcc"));
    require_executable(&prefix.with_file_name("arm-rockchip830-linux-uclibcgnueabihf-g++"));

    let out_dir = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let toolchain_file = out_dir.join("media-worker-toolchain.cmake");
    let sysroot = toolchain_dir.join("arm-rockchip830-linux-uclibcgnueabihf/sysroot");
    let toolchain_contents = format!(
        "set(CMAKE_SYSTEM_NAME Linux)\n\
         set(CMAKE_SYSTEM_PROCESSOR arm)\n\
         set(CMAKE_C_COMPILER \"{}-gcc\")\n\
         set(CMAKE_CXX_COMPILER \"{}-g++\")\n\
         set(CMAKE_AR \"{}-ar\")\n\
         set(CMAKE_RANLIB \"{}-ranlib\")\n\
         set(CMAKE_STRIP \"{}-strip\")\n\
         set(CMAKE_SYSROOT \"{}\")\n\
         set(CMAKE_LINK_DEPENDS_NO_SHARED TRUE)\n\
         set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n\
         set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n\
         set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n\
         set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n",
        prefix.display(),
        prefix.display(),
        prefix.display(),
        prefix.display(),
        prefix.display(),
        sysroot.display()
    );
    fs::write(&toolchain_file, toolchain_contents).expect("write generated CMake toolchain");

    let profile = match env::var("PROFILE").as_deref() {
        Ok("release") => "Release",
        _ => "Debug",
    };
    let cpp_out = workspace_root
        .join("target/cpp")
        .join(target)
        .join(profile.to_ascii_lowercase());
    let json_include = workspace_root.join("3rdparty/nlohmann_json/include");
    let installed = cmake::Config::new(workspace_root.join("media_worker"))
        .generator("Ninja")
        .profile(profile)
        .out_dir(&cpp_out)
        .define("CMAKE_TOOLCHAIN_FILE", &toolchain_file)
        .define("MEDIA_WORKER_SDK_ROOT", &sdk_root)
        .define("MEDIA_WORKER_JSON_INCLUDE_DIR", &json_include)
        .define("MEDIA_WORKER_BUILD_RUNTIME", "ON")
        .define("MEDIA_WORKER_BUILD_TESTS", "OFF")
        .build();

    let worker_source = installed.join("bin/media_worker");
    if !worker_source.is_file() {
        panic!("CMake did not install {}", worker_source.display());
    }
    let target_dir = env::var_os("CARGO_TARGET_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| workspace_root.join("target"));
    let cargo_profile = env::var("PROFILE").unwrap();
    let destination_dir = target_dir.join(target).join(cargo_profile);
    fs::create_dir_all(&destination_dir).expect("create Cargo target profile directory");
    let destination = destination_dir.join("media_worker");
    fs::copy(&worker_source, &destination).expect("copy media_worker beside daemon");
    println!(
        "cargo:warning=media_worker artifact: {}",
        destination.display()
    );

    let ai_deps = env::var_os("AIPC_AI_DEPS_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| workspace_root.join("target/ai-deps"));
    let ai_out = workspace_root
        .join("target/cpp-ai")
        .join(target)
        .join(profile.to_ascii_lowercase());
    let ai_installed = cmake::Config::new(workspace_root.join("ai_worker"))
        .generator("Ninja")
        .profile(profile)
        .out_dir(&ai_out)
        .define("CMAKE_TOOLCHAIN_FILE", &toolchain_file)
        .define("AI_WORKER_DEPS_DIR", &ai_deps)
        .define("AI_WORKER_SDK_ROOT", &sdk_root)
        .define(
            "AI_WORKER_MPI_ROOT",
            workspace_root.join("3rdparty/luckfox_pico_rkmpi_example"),
        )
        .define("AI_WORKER_JSON_INCLUDE_DIR", &json_include)
        .define("AI_WORKER_ENABLE_VISIONG", "ON")
        .build();
    let ai_source = ai_installed.join("bin/ai_worker");
    if !ai_source.is_file() {
        panic!("CMake did not install {}", ai_source.display());
    }
    let ai_destination = destination_dir.join("ai_worker");
    fs::copy(&ai_source, &ai_destination).expect("copy ai_worker beside daemon");
    println!(
        "cargo:warning=ai_worker artifact: {}",
        ai_destination.display()
    );
}

fn require_executable(path: &Path) {
    if !path.is_file() {
        panic!(
            "required RV1106 compiler not found at {}; set AIPC_SDK_ROOT",
            path.display()
        );
    }
}
