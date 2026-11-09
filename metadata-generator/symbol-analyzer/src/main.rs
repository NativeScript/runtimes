use std::collections::BTreeSet;
use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use ns_metadata_symbols::analyze_source;
use rayon::prelude::*;

#[derive(Default)]
struct Options {
    output: Option<PathBuf>,
    include_whitelists: Vec<PathBuf>,
    keep_modules: Vec<String>,
    inputs: Vec<PathBuf>,
    no_default_modules: bool,
}

fn usage() -> &'static str {
    "Usage: ns-metadata-symbols [options] <bundle-or-directory>...\n\
     Options:\n\
       --output <path>             Write an mdg whitelist instead of stdout\n\
       --include-whitelist <path>  Merge an existing NativeScript whitelist\n\
       --keep-module <name>        Keep an entire native module/framework\n\
       --no-default-modules        Do not automatically keep Foundation/Runtime\n\
       --help                      Show this help"
}

fn parse_options() -> Result<Options, String> {
    let mut options = Options::default();
    let mut args = env::args_os().skip(1);
    while let Some(arg) = args.next() {
        match arg.to_string_lossy().as_ref() {
            "--output" => {
                options.output = Some(args.next().ok_or("--output requires a path")?.into());
            }
            "--include-whitelist" => options.include_whitelists.push(
                args.next()
                    .ok_or("--include-whitelist requires a path")?
                    .into(),
            ),
            "--keep-module" => options.keep_modules.push(
                args.next()
                    .ok_or("--keep-module requires a name")?
                    .to_string_lossy()
                    .into_owned(),
            ),
            "--no-default-modules" => options.no_default_modules = true,
            "--help" | "-h" => return Err(usage().to_owned()),
            value if value.starts_with('-') => return Err(format!("Unknown option: {value}")),
            _ => options.inputs.push(arg.into()),
        }
    }

    if options.inputs.is_empty() {
        return Err("At least one bundle or directory is required".to_owned());
    }
    Ok(options)
}

fn is_source_file(path: &Path) -> bool {
    matches!(
        path.extension().and_then(|value| value.to_str()),
        Some("js" | "mjs" | "cjs" | "jsx" | "ts" | "mts" | "cts" | "tsx")
    )
}

fn collect_files(path: &Path, output: &mut Vec<PathBuf>) -> io::Result<()> {
    if path.is_file() {
        if is_source_file(path) {
            output.push(path.to_owned());
        }
        return Ok(());
    }

    for entry in fs::read_dir(path)? {
        let entry = entry?;
        let file_type = entry.file_type()?;
        if file_type.is_symlink() {
            continue;
        }
        if file_type.is_dir() {
            collect_files(&entry.path(), output)?;
        } else if file_type.is_file() && is_source_file(&entry.path()) {
            output.push(entry.path());
        }
    }
    Ok(())
}

fn run(options: Options) -> Result<(), String> {
    let mut files = Vec::new();
    for input in &options.inputs {
        collect_files(input, &mut files)
            .map_err(|error| format!("Unable to read {}: {error}", input.display()))?;
    }
    files.sort();
    files.dedup();
    if files.is_empty() {
        return Err("No JavaScript or TypeScript bundle files were found".to_owned());
    }

    let analyses: Vec<_> = files
        .par_iter()
        .map(|path| {
            let bytes = fs::read(path)
                .map_err(|error| format!("Unable to read {}: {error}", path.display()))?;
            let analysis = match std::str::from_utf8(&bytes) {
                Ok(source) => analyze_source(path, source),
                Err(_) => ns_metadata_symbols::Analysis {
                    diagnostics: 1,
                    bailed_out: true,
                    ..ns_metadata_symbols::Analysis::default()
                },
            };
            Ok::<_, String>((path, analysis))
        })
        .collect::<Result<_, _>>()?;

    let mut lines = BTreeSet::new();
    for whitelist in &options.include_whitelists {
        let contents = fs::read_to_string(whitelist)
            .map_err(|error| format!("Unable to read {}: {error}", whitelist.display()))?;
        lines.extend(
            contents
                .lines()
                .map(str::trim)
                .filter(|line| {
                    !line.is_empty() && !line.starts_with('#') && !line.starts_with("//")
                })
                .map(str::to_owned),
        );
    }

    if !options.no_default_modules {
        lines.insert("Foundation:*".to_owned());
        lines.insert("Runtime:*".to_owned());
    }
    lines.extend(
        options
            .keep_modules
            .iter()
            .map(|module| format!("{module}:*")),
    );

    let mut diagnostics = 0;
    let mut bailed_out = false;
    for (_, analysis) in &analyses {
        diagnostics += analysis.diagnostics;
        bailed_out |= analysis.bailed_out;
        lines.extend(analysis.symbols.iter().map(|symbol| format!("*:{symbol}")));
    }

    if bailed_out {
        // Filtering must fail open. A malformed or unsupported bundle should
        // never cause required native metadata to be removed.
        lines.insert("*:*".to_owned());
    }

    let mut rendered = String::from("# Generated by ns-metadata-symbols; do not edit.\n");
    for line in &lines {
        rendered.push_str(line);
        rendered.push('\n');
    }

    match options.output {
        Some(path) => fs::write(&path, rendered)
            .map_err(|error| format!("Unable to write {}: {error}", path.display()))?,
        None => io::stdout()
            .write_all(rendered.as_bytes())
            .map_err(|error| format!("Unable to write stdout: {error}"))?,
    }

    eprintln!(
        "Analyzed {} file(s) on {} worker(s): {} symbol pattern(s), {} diagnostic(s){}",
        files.len(),
        rayon::current_num_threads(),
        lines.len(),
        diagnostics,
        if bailed_out {
            "; filtering disabled (fail-open)"
        } else {
            ""
        }
    );
    Ok(())
}

fn main() -> ExitCode {
    match parse_options().and_then(run) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("{message}");
            ExitCode::FAILURE
        }
    }
}
