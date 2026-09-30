#![forbid(unsafe_code)]

use clap::Parser;

fn main() -> std::process::ExitCode {
    meshcore_mailboxd::cli::main(meshcore_mailboxd::cli::Cli::parse())
}
