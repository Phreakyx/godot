---
type: Mistake
title: A Linux build disk kept as an ext4 image on the NTFS drive
description: The Linux builds ran on an ext4 image file stored on the Windows NTFS drive; the ntfs driver failed an allocation while mapping the fragmented sparse image, the loop device returned I/O errors and ext4 went read-only mid-build. Linux builds go on a real Linux partition.
tags: [mistake, linux, build, disk]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T22:00:00Z }
sources:
  - id: setup
    resource: ../../devtools/setup/setup_dev_cachyos.sh
    title: the NTFS mount warning in the CachyOS setup script
---

# What happened

- At first the CachyOS session's build directory was an ext4 file system
  inside a sparse image file kept on the Windows NTFS drive, mounted
  through a loop device. The setup
  script's NTFS check does not see this: the repository's own file system
  is ext4.
- Mid-build, the out-of-tree ntfs driver failed an order-9 `GFP_NOIO`
  allocation while it mapped the fragmented image, the loop device
  returned I/O errors, the ext4 journal aborted and the file system went
  read-only. The build stopped halfway and had to start again.

# Rule

- Linux builds go on a real Linux partition (the session grew the ext4
  root to 159 GB). No image files on NTFS, no NTFS mounts.
- Plan the space first: about 105 GiB for the source, the SDKs and the
  five builds, plus about 40 GB per benchmark compiler's objects.
