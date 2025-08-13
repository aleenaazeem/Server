# Distributed File Store (DFS) — COMP-8567 Project

A simple distributed file storage system with one main server (**S1**) and three auxiliary servers (**S2**, **S3**, **S4**) for storing specific file types. Includes a lightweight command-line client for uploading, downloading, deleting, listing, and archiving files.

---

## 🚀 Features

- **Upload files** (`uploadf`) — send 1–3 files to the cluster.
- **Download files** (`downlf`) — retrieve one or more files by path.
- **Remove files** (`removef`) — delete files remotely.
- **Download tar archives** (`downltar`) — download `.c`, `.pdf`, or `.txt` files as a single `.tar`.
- **Display file names** (`dispfnames`) — list file names across all servers for a given directory.
- **Type-based routing:**
  - `.c` → S1
  - `.pdf` → S2
  - `.txt` → S3
  - `.zip` → S4

---

## 📂 Project Structure

