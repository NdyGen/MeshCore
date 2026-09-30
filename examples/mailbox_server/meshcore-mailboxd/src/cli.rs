//! The command line: `serve` for the radio, the rest for the Pi admin.

use std::io;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Args, Parser, Subcommand};

use crate::domain::model::{Limits, LimitsError};
use crate::mailbox::{AdminError, Mailbox};
use crate::protocol::{self, FieldError};
use crate::session::Session;
use crate::storage::{SqliteStorage, StorageError};
use crate::system::{Clock, FakeClock, SystemClock};
use crate::transport::{self, SerialEvents, ServeError};

#[derive(Debug, Parser)]
#[command(
    name = "meshcore-mailboxd",
    about = "meshcore-mailboxd: message store of the MeshCore DM mailbox",
    infer_long_args = true
)]
pub struct Cli {
    #[arg(long, default_value = "/var/lib/meshcore-mailboxd/mailbox.db")]
    pub db: PathBuf,
    #[arg(short, long)]
    pub verbose: bool,
    #[command(subcommand)]
    pub command: Command,
}

#[derive(Debug, Subcommand)]
pub enum Command {
    /// answer the radio
    Serve(Serve),
    /// add an owner or change its limits; prints K_owner
    OwnerAdd(OwnerAdd),
    /// list owners (without keys)
    OwnerList,
    /// deny a depositor for one owner
    Deny(Depositor),
    /// undeny a depositor for one owner
    Undeny(Depositor),
}

#[derive(Debug, Args)]
#[command(group(clap::ArgGroup::new("where").required(true).args(["port", "stdio"])))]
pub struct Serve {
    /// serial port of the mailbox radio
    #[arg(long)]
    pub port: Option<String>,
    /// lines on stdin, replies on stdout (tests)
    #[arg(long)]
    pub stdio: bool,
    /// with --stdio: clock set by '@MBX TIME <unix>'
    #[arg(long, conflicts_with = "port")]
    pub fake_clock: bool,
}

#[derive(Debug, Args)]
pub struct OwnerAdd {
    /// owner public key, 64 hex
    #[arg(value_name = "PUB")]
    pub pubkey: String,
    /// 32 hex; default: keep the current key or generate one
    #[arg(long)]
    pub k_owner: Option<String>,
    #[arg(long, default_value_t = 7, allow_negative_numbers = true)]
    pub ttl_days: i64,
    #[arg(long, default_value_t = 30, allow_negative_numbers = true)]
    pub sync_days: i64,
    #[arg(long, default_value_t = 20, allow_negative_numbers = true)]
    pub quota: i64,
}

#[derive(Debug, Args)]
pub struct Depositor {
    /// owner prefix (8 hex) or public key (64 hex)
    pub owner: String,
    /// depositor public key, 64 hex
    #[arg(value_name = "PUB")]
    pub pubkey: String,
}

#[derive(Debug, thiserror::Error)]
pub enum CliError {
    #[error(transparent)]
    Argument(#[from] FieldError),
    #[error(transparent)]
    Limits(#[from] LimitsError),
    #[error(transparent)]
    Admin(#[from] AdminError),
    #[error(transparent)]
    Storage(#[from] StorageError),
    #[error(transparent)]
    Serve(#[from] ServeError),
    #[error("serial port: {0}")]
    Serial(#[from] serialport::Error),
}

impl CliError {
    /// 2 for what the operator typed wrong; 1 for everything else.
    pub fn exit_code(&self) -> u8 {
        match self {
            CliError::Argument(_) | CliError::Limits(_) => 2,
            CliError::Admin(AdminError::PrefixTaken(_) | AdminError::UnknownOwner(_)) => 2,
            _ => 1,
        }
    }
}

pub fn main(cli: Cli) -> ExitCode {
    StderrLogger::install(cli.verbose);
    match run(cli) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("meshcore-mailboxd: {e}");
            ExitCode::from(e.exit_code())
        }
    }
}

pub fn run(cli: Cli) -> Result<(), CliError> {
    let now = SystemClock.now();
    match cli.command {
        Command::Serve(s) => serve(&cli.db, &s),
        Command::OwnerAdd(a) => {
            let pubkey = protocol::parse_pubkey(&a.pubkey)?;
            let k_owner = a
                .k_owner
                .as_deref()
                .filter(|k| !k.is_empty())
                .map(protocol::parse_k_owner)
                .transpose()?;
            let limits = Limits::new(a.ttl_days, a.sync_days, a.quota)?;
            let k = open(&cli.db)?.owner_add(now, &pubkey, k_owner, limits)?;
            println!("owner {} k_owner {k}", pubkey.owner4());
            Ok(())
        }
        Command::OwnerList => {
            for o in open(&cli.db)?.owners()? {
                let store_id = o
                    .store_id
                    .map_or_else(|| "-".to_string(), |s| format!("{s:08x}"));
                let l = o.limits;
                println!(
                    "owner {} owner4={} ttl_days={} sync_days={} quota={} store_id={store_id}",
                    o.pubkey,
                    o.owner4(),
                    l.ttl_days(),
                    l.sync_days(),
                    l.quota()
                );
            }
            Ok(())
        }
        Command::Deny(d) => {
            let (owner4, pubkey) = (
                protocol::parse_owner(&d.owner)?,
                protocol::parse_pubkey(&d.pubkey)?,
            );
            Ok(open(&cli.db)?.deny(now, &owner4, &pubkey)?)
        }
        Command::Undeny(d) => {
            let (owner4, pubkey) = (
                protocol::parse_owner(&d.owner)?,
                protocol::parse_pubkey(&d.pubkey)?,
            );
            Ok(open(&cli.db)?.undeny(&owner4, &pubkey)?)
        }
    }
}

fn open(db: &std::path::Path) -> Result<Mailbox<SqliteStorage>, StorageError> {
    Ok(Mailbox::new(SqliteStorage::open(db)?))
}

fn serve(db: &std::path::Path, s: &Serve) -> Result<(), CliError> {
    let mailbox = open(db)?;
    if let Some(port) = &s.port {
        let port = transport::open_serial(port)?;
        let mut out = port.try_clone()?;
        let mut session = Session::new(mailbox, SystemClock);
        return Ok(transport::serve(
            &mut session,
            SerialEvents::new(port),
            &mut out,
        )?);
    }
    let events = transport::stdin_events(io::stdin());
    let mut out = io::stdout().lock();
    if s.fake_clock {
        // the simulator sets the clock with `@MBX TIME` before its first request
        transport::serve(
            &mut Session::new(mailbox, FakeClock::new(0)),
            events,
            &mut out,
        )?;
    } else {
        transport::serve(&mut Session::new(mailbox, SystemClock), events, &mut out)?;
    }
    Ok(())
}

/// `2026-09-30 12:00:00 INFO message` on stderr, UTC; journald adds its own timestamp on the Pi.
struct StderrLogger;

impl StderrLogger {
    fn install(verbose: bool) {
        static LOGGER: StderrLogger = StderrLogger;
        if log::set_logger(&LOGGER).is_ok() {
            log::set_max_level(if verbose {
                log::LevelFilter::Debug
            } else {
                log::LevelFilter::Info
            });
        }
    }
}

impl log::Log for StderrLogger {
    fn enabled(&self, _: &log::Metadata<'_>) -> bool {
        true
    }

    fn log(&self, record: &log::Record<'_>) {
        eprintln!(
            "{} {} {}",
            utc(SystemClock.now()),
            record.level(),
            record.args()
        );
    }

    fn flush(&self) {}
}

/// `YYYY-MM-DD HH:MM:SS` for a Unix time (civil-from-days, H. Hinnant).
fn utc(t: i64) -> String {
    let (days, secs) = (t.div_euclid(86_400), t.rem_euclid(86_400));
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}-{month:02}-{day:02} {:02}:{:02}:{:02}",
        secs / 3600,
        secs / 60 % 60,
        secs % 60
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utc_timestamps() {
        assert_eq!(utc(0), "1970-01-01 00:00:00");
        assert_eq!(utc(951_782_400), "2000-02-29 00:00:00");
        assert_eq!(utc(1_790_000_000), "2026-09-21 14:13:20");
        assert_eq!(utc(4_102_444_799), "2099-12-31 23:59:59");
    }

    #[test]
    fn cli_definition_is_consistent() {
        use clap::CommandFactory;
        Cli::command().debug_assert();
    }

    #[test]
    fn abbreviated_long_options_like_argparse() {
        let cli = Cli::try_parse_from([
            "meshcore-mailboxd",
            "--db",
            "x.db",
            "serve",
            "--std",
            "--fake",
        ])
        .unwrap();
        assert!(matches!(
            cli.command,
            Command::Serve(Serve {
                stdio: true,
                fake_clock: true,
                port: None
            })
        ));
    }
}
