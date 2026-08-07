use std::path::PathBuf;
use tokio::io::{AsyncReadExt, AsyncWrite, AsyncWriteExt};

struct ZipEntry {
    name: Vec<u8>,
    crc: u32,
    size: u64,
    offset: u64,
}

pub(super) async fn write_zip64<W: AsyncWrite + Unpin>(
    mut writer: W,
    files: Vec<(String, PathBuf)>,
) -> anyhow::Result<()> {
    let mut entries = Vec::new();
    let mut offset = 0_u64;
    for (name, path) in files {
        let safe_name: Vec<u8> = name
            .bytes()
            .map(|byte| {
                if matches!(byte, b'/' | b'\\') {
                    b'_'
                } else {
                    byte
                }
            })
            .collect();
        let local_offset = offset;
        let mut header = Vec::new();
        push_u32(&mut header, 0x04034b50);
        push_u16(&mut header, 45);
        push_u16(&mut header, 0x0008);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u32(&mut header, 0);
        push_u32(&mut header, u32::MAX);
        push_u32(&mut header, u32::MAX);
        push_u16(&mut header, safe_name.len() as u16);
        push_u16(&mut header, 20);
        header.extend_from_slice(&safe_name);
        push_u16(&mut header, 0x0001);
        push_u16(&mut header, 16);
        push_u64(&mut header, 0);
        push_u64(&mut header, 0);
        writer.write_all(&header).await?;
        offset += header.len() as u64;
        let mut file = tokio::fs::File::open(path).await?;
        let mut buffer = vec![0_u8; 64 * 1024];
        let mut hasher = crc32fast::Hasher::new();
        let mut size = 0_u64;
        loop {
            let count = file.read(&mut buffer).await?;
            if count == 0 {
                break;
            }
            hasher.update(&buffer[..count]);
            writer.write_all(&buffer[..count]).await?;
            size += count as u64;
            offset += count as u64;
        }
        let crc = hasher.finalize();
        let mut descriptor = Vec::new();
        push_u32(&mut descriptor, 0x08074b50);
        push_u32(&mut descriptor, crc);
        push_u64(&mut descriptor, size);
        push_u64(&mut descriptor, size);
        writer.write_all(&descriptor).await?;
        offset += descriptor.len() as u64;
        entries.push(ZipEntry {
            name: safe_name,
            crc,
            size,
            offset: local_offset,
        });
    }
    let central_offset = offset;
    for entry in &entries {
        let mut header = Vec::new();
        push_u32(&mut header, 0x02014b50);
        push_u16(&mut header, 45);
        push_u16(&mut header, 45);
        push_u16(&mut header, 0x0008);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u32(&mut header, entry.crc);
        push_u32(&mut header, u32::MAX);
        push_u32(&mut header, u32::MAX);
        push_u16(&mut header, entry.name.len() as u16);
        push_u16(&mut header, 28);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u16(&mut header, 0);
        push_u32(&mut header, 0);
        push_u32(&mut header, u32::MAX);
        header.extend_from_slice(&entry.name);
        push_u16(&mut header, 0x0001);
        push_u16(&mut header, 24);
        push_u64(&mut header, entry.size);
        push_u64(&mut header, entry.size);
        push_u64(&mut header, entry.offset);
        writer.write_all(&header).await?;
        offset += header.len() as u64;
    }
    let central_size = offset - central_offset;
    let zip64_offset = offset;
    let mut ending = Vec::new();
    push_u32(&mut ending, 0x06064b50);
    push_u64(&mut ending, 44);
    push_u16(&mut ending, 45);
    push_u16(&mut ending, 45);
    push_u32(&mut ending, 0);
    push_u32(&mut ending, 0);
    push_u64(&mut ending, entries.len() as u64);
    push_u64(&mut ending, entries.len() as u64);
    push_u64(&mut ending, central_size);
    push_u64(&mut ending, central_offset);
    push_u32(&mut ending, 0x07064b50);
    push_u32(&mut ending, 0);
    push_u64(&mut ending, zip64_offset);
    push_u32(&mut ending, 1);
    push_u32(&mut ending, 0x06054b50);
    push_u16(&mut ending, 0);
    push_u16(&mut ending, 0);
    push_u16(&mut ending, u16::MAX);
    push_u16(&mut ending, u16::MAX);
    push_u32(&mut ending, u32::MAX);
    push_u32(&mut ending, u32::MAX);
    push_u16(&mut ending, 0);
    writer.write_all(&ending).await?;
    writer.shutdown().await?;
    Ok(())
}

fn push_u16(output: &mut Vec<u8>, value: u16) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn push_u32(output: &mut Vec<u8>, value: u32) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn push_u64(output: &mut Vec<u8>, value: u64) {
    output.extend_from_slice(&value.to_le_bytes());
}
