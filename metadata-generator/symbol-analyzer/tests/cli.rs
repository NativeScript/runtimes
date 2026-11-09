use std::fs;
use std::process::Command;

#[test]
fn writes_deterministic_mdg_and_merges_manual_whitelist() {
    let root = tempfile::tempdir().unwrap();
    let bundle = root.path().join("bundle.js");
    let manual = root.path().join("whitelist.mdg");
    let output = root.path().join("generated.mdg");
    let second_output = root.path().join("generated-again.mdg");
    fs::write(&bundle, "new UIView(); CGRectMake(0, 0, 1, 1);").unwrap();
    fs::write(&manual, "# existing rules\nAppKit:NSView\n").unwrap();

    let status = Command::new(env!("CARGO_BIN_EXE_ns-metadata-symbols"))
        .args(["--output"])
        .arg(&output)
        .args(["--include-whitelist"])
        .arg(&manual)
        .arg(&bundle)
        .status()
        .unwrap();
    assert!(status.success());

    let result = fs::read_to_string(output).unwrap();
    assert!(result.contains("*:CGRectMake\n"));
    assert!(result.contains("*:UIView\n"));
    assert!(result.contains("AppKit:NSView\n"));
    assert!(result.contains("Foundation:*\n"));
    assert!(result.contains("Runtime:*\n"));

    let second_status = Command::new(env!("CARGO_BIN_EXE_ns-metadata-symbols"))
        .args(["--output"])
        .arg(&second_output)
        .args(["--include-whitelist"])
        .arg(&manual)
        .arg(&bundle)
        .status()
        .unwrap();
    assert!(second_status.success());
    assert_eq!(result, fs::read_to_string(second_output).unwrap());
}

#[test]
fn non_utf8_input_disables_filtering_instead_of_failing_the_build() {
    let root = tempfile::tempdir().unwrap();
    let bundle = root.path().join("bundle.js");
    let output = root.path().join("generated.mdg");
    fs::write(&bundle, [0xff, 0xfe]).unwrap();

    let status = Command::new(env!("CARGO_BIN_EXE_ns-metadata-symbols"))
        .args(["--output"])
        .arg(&output)
        .arg(&bundle)
        .status()
        .unwrap();
    assert!(status.success());
    assert!(fs::read_to_string(output).unwrap().contains("*:*\n"));
}

#[test]
fn merges_main_bundle_and_worker_chunk_symbols() {
    let root = tempfile::tempdir().unwrap();
    let chunks = root.path().join("chunks");
    let output = root.path().join("generated.mdg");
    fs::create_dir(&chunks).unwrap();
    fs::write(chunks.join("main.min.js"), "new UIView;new NSWindow;").unwrap();
    fs::write(
        chunks.join("worker.min.js"),
        "onmessage=()=>NSLocale.currentLocale;",
    )
    .unwrap();

    let status = Command::new(env!("CARGO_BIN_EXE_ns-metadata-symbols"))
        .args(["--output"])
        .arg(&output)
        .arg(&chunks)
        .status()
        .unwrap();
    assert!(status.success());

    let result = fs::read_to_string(output).unwrap();
    assert!(result.contains("*:UIView\n"));
    assert!(result.contains("*:NSWindow\n"));
    assert!(result.contains("*:NSLocale\n"));
    assert!(!result.contains("*:*\n"));
}

#[test]
fn dynamic_global_alias_disables_cli_filtering() {
    let root = tempfile::tempdir().unwrap();
    let bundle = root.path().join("bundle.js");
    let output = root.path().join("generated.mdg");
    fs::write(&bundle, "const nativeGlobal=globalThis;nativeGlobal[name]").unwrap();

    let status = Command::new(env!("CARGO_BIN_EXE_ns-metadata-symbols"))
        .args(["--output"])
        .arg(&output)
        .arg(&bundle)
        .status()
        .unwrap();
    assert!(status.success());
    assert!(fs::read_to_string(output).unwrap().contains("*:*\n"));
}
