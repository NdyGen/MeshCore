#![forbid(unsafe_code)]

use clap::Parser;

fn main() -> std::process::ExitCode {
    mbxd::cli::main(mbxd::cli::Cli::parse())
}
