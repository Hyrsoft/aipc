use crate::model::PersistentState;
use std::path::{Path, PathBuf};

#[derive(Clone)]
pub struct StateStore {
    path: PathBuf,
}

impl StateStore {
    pub fn new(data_dir: &Path) -> Self {
        Self {
            path: data_dir.join("state.json"),
        }
    }

    pub async fn load(&self) -> anyhow::Result<Option<PersistentState>> {
        match tokio::fs::read(&self.path).await {
            Ok(data) => Ok(Some(serde_json::from_slice(&data)?)),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(error) => Err(error.into()),
        }
    }

    pub async fn save(&self, state: &PersistentState) -> anyhow::Result<()> {
        if let Some(parent) = self.path.parent() {
            tokio::fs::create_dir_all(parent).await?;
        }
        let temporary = self.path.with_extension("json.tmp");
        let data = serde_json::to_vec_pretty(state)?;
        tokio::fs::write(&temporary, data).await?;
        tokio::fs::rename(&temporary, &self.path).await?;
        Ok(())
    }
}
