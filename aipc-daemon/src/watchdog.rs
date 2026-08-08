use crate::config::WatchdogConfig;
use anyhow::Context;
use std::fs::{File, OpenOptions};
use std::os::fd::AsRawFd;
use std::sync::Arc;
use std::time::Duration;
use tokio::task::JoinHandle;
use tokio::time::MissedTickBehavior;
use tracing::{error, info, warn};

// Linux uapi: _IOR('W', 5, int) and _IOWR('W', 6, int). The values are
// identical on the host and RV1106 because the payload is a 32-bit int.
const WDIOC_KEEPALIVE: libc::c_ulong = 0x8004_5705;
const WDIOC_SETTIMEOUT: libc::c_ulong = 0xc004_5706;

pub struct Watchdog {
    _task: Option<JoinHandle<()>>,
}

impl Watchdog {
    pub fn start(config: &WatchdogConfig) -> anyhow::Result<Self> {
        if !config.enabled {
            info!("hardware watchdog disabled by configuration");
            return Ok(Self { _task: None });
        }

        let file = match OpenOptions::new().write(true).open(&config.device) {
            Ok(file) => file,
            Err(error) if !config.required => {
                warn!(
                    device = %config.device.display(),
                    %error,
                    "hardware watchdog unavailable; continuing without automatic board reset"
                );
                return Ok(Self { _task: None });
            }
            Err(error) => {
                return Err(error).with_context(|| {
                    format!("open required watchdog device {}", config.device.display())
                });
            }
        };

        let mut timeout = i32::try_from(config.timeout_sec).context("watchdog timeout overflow")?;
        if unsafe { libc::ioctl(file.as_raw_fd(), WDIOC_SETTIMEOUT, &mut timeout) } < 0 {
            let error = std::io::Error::last_os_error();
            return Err(error).context("set hardware watchdog timeout");
        }

        let file = Arc::new(file);
        ping(&file).context("initial hardware watchdog keepalive")?;
        let feed_interval = Duration::from_millis(config.feed_interval_ms);
        let device = config.device.clone();
        let task_file = file.clone();
        let task = tokio::spawn(async move {
            let mut interval = tokio::time::interval(feed_interval);
            interval.set_missed_tick_behavior(MissedTickBehavior::Delay);
            interval.tick().await;
            loop {
                interval.tick().await;
                if let Err(error) = ping(&task_file) {
                    error!(device = %device.display(), %error, "hardware watchdog keepalive failed");
                    break;
                }
            }
        });
        info!(
            device = %config.device.display(),
            requested_timeout_sec = config.timeout_sec,
            actual_timeout_sec = timeout,
            feed_interval_ms = config.feed_interval_ms,
            "hardware watchdog armed"
        );
        Ok(Self { _task: Some(task) })
    }
}

fn ping(file: &File) -> std::io::Result<()> {
    if unsafe { libc::ioctl(file.as_raw_fd(), WDIOC_KEEPALIVE, 0) } < 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn disabled_watchdog_never_opens_a_device() {
        let config = WatchdogConfig {
            enabled: false,
            required: true,
            device: "/definitely/missing/watchdog".into(),
            ..WatchdogConfig::default()
        };
        assert!(Watchdog::start(&config).is_ok());
    }
}
