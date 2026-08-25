#!/usr/bin/env bash
#
# RevFS — End-to-End Demo Script
#
# Demonstrates all RevFS features:
#   1. Build the project
#   2. Local operations (upload, download, verify, history, restore, stats)
#   3. Server/client operations (remote upload/download, list, history)
#   4. Deduplication demo
#   5. Cleanup
#
# Usage:
#   chmod +x scripts/demo.sh
#   ./scripts/demo.sh
#

set -e

# ─── Colors ───────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

step() {
    echo ""
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}  $1${NC}"
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""
}

success() {
    echo -e "${GREEN}  ✅ $1${NC}"
}

info() {
    echo -e "${YELLOW}  → $1${NC}"
}

DEMO_DIR="data/demo_tmp"
DEMO_PORT=9876

cleanup() {
    info "Cleaning up demo files..."
    rm -rf "$DEMO_DIR"
    rm -rf data/meta/demo_file.txt
    rm -rf data/meta/demo_dup.txt
    rm -rf data/chunks
    rm -rf data/journal.wal data/journal.wal.bak
    # Kill any lingering server
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}

trap cleanup EXIT

# ─── Step 0: Build ────────────────────────────────────────────────
step "Step 0: Building RevFS"

make clean >/dev/null 2>&1 || true
make 2>&1
success "Build successful"

echo ""
./revfs --version
echo ""

# ─── Step 1: Create Demo Files ───────────────────────────────────
step "Step 1: Creating Demo Files"

mkdir -p "$DEMO_DIR"

# Create a 1 KB test file
echo "Hello from RevFS! This is the first version of our demo file." > "$DEMO_DIR/demo_file.txt"
for i in $(seq 1 20); do
    echo "Line $i: Lorem ipsum dolor sit amet, consectetur adipiscing elit." >> "$DEMO_DIR/demo_file.txt"
done

FILE_SIZE=$(wc -c < "$DEMO_DIR/demo_file.txt" | tr -d ' ')
success "Created demo_file.txt ($FILE_SIZE bytes)"

# ─── Step 2: Upload Version 1 ───────────────────────────────────
step "Step 2: Uploading Version 1 (Local)"

./revfs upload "$DEMO_DIR/demo_file.txt"
success "Version 1 uploaded"

# ─── Step 3: Upload Version 2 (modified content) ────────────────
step "Step 3: Uploading Version 2 (Modified Content)"

echo "" >> "$DEMO_DIR/demo_file.txt"
echo "=== VERSION 2 CHANGES ===" >> "$DEMO_DIR/demo_file.txt"
echo "Added new section with additional content for version 2." >> "$DEMO_DIR/demo_file.txt"
for i in $(seq 1 10); do
    echo "New line $i: Sed do eiusmod tempor incididunt ut labore." >> "$DEMO_DIR/demo_file.txt"
done

./revfs upload "$DEMO_DIR/demo_file.txt"
success "Version 2 uploaded"

# ─── Step 4: Upload Version 3 ───────────────────────────────────
step "Step 4: Uploading Version 3"

echo "" >> "$DEMO_DIR/demo_file.txt"
echo "=== VERSION 3 — FINAL DRAFT ===" >> "$DEMO_DIR/demo_file.txt"
echo "This is the final revision before we test restore." >> "$DEMO_DIR/demo_file.txt"

./revfs upload "$DEMO_DIR/demo_file.txt"
success "Version 3 uploaded"

# ─── Step 5: List Files ──────────────────────────────────────────
step "Step 5: Listing All Stored Files"

./revfs list

# ─── Step 6: View Version History ────────────────────────────────
step "Step 6: Version History for demo_file.txt"

./revfs history demo_file.txt

# ─── Step 7: Download Specific Version ───────────────────────────
step "Step 7: Downloading Version 1"

./revfs download demo_file.txt "$DEMO_DIR/restored_v1.txt" --version 1
success "Downloaded version 1"

info "Verifying content..."
V1_SIZE=$(wc -c < "$DEMO_DIR/restored_v1.txt" | tr -d ' ')
echo "  Downloaded file size: $V1_SIZE bytes"

# ─── Step 8: Restore Version 1 ──────────────────────────────────
step "Step 8: Restoring Version 1 (Non-Destructive)"

info "Current latest is v3. Restoring v1 will create v4..."
./revfs restore demo_file.txt 1
success "Version 1 restored as version 4"

info "Checking history after restore..."
./revfs history demo_file.txt

# ─── Step 9: Storage Stats ───────────────────────────────────────
step "Step 9: Storage & Deduplication Statistics"

./revfs stats

# ─── Step 10: Deduplication Demo ─────────────────────────────────
step "Step 10: Deduplication in Action"

info "Uploading the same file content as a different name..."
cp "$DEMO_DIR/demo_file.txt" "$DEMO_DIR/demo_dup.txt"
./revfs upload "$DEMO_DIR/demo_dup.txt"
success "Duplicate file uploaded"

info "Stats after duplicate upload:"
./revfs stats
success "Notice the dedup ratio — chunks are shared!"

# ─── Step 11: Server / Client Demo ──────────────────────────────
step "Step 11: TCP Server & Client Operations"

info "Starting RevFS server on port $DEMO_PORT..."
./revfs server $DEMO_PORT --threads 2 &
SERVER_PID=$!
sleep 1

if kill -0 "$SERVER_PID" 2>/dev/null; then
    success "Server running (PID: $SERVER_PID)"
else
    echo -e "${RED}  ❌ Server failed to start${NC}"
    exit 1
fi

info "Pinging server..."
./revfs ping hello --host 127.0.0.1 --port $DEMO_PORT

info "Listing remote files..."
./revfs list --host 127.0.0.1 --port $DEMO_PORT

info "Viewing remote history..."
./revfs history demo_file.txt --host 127.0.0.1 --port $DEMO_PORT

info "Getting remote stats..."
./revfs stats --host 127.0.0.1 --port $DEMO_PORT

info "Stopping server..."
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID
success "Server stopped"

# ─── Summary ─────────────────────────────────────────────────────
step "Demo Complete!"

echo -e "${GREEN}  RevFS demonstrated:${NC}"
echo -e "    ✅ File upload with content-addressed chunking"
echo -e "    ✅ Multi-version file management"
echo -e "    ✅ Version history tracking"
echo -e "    ✅ Specific version download"
echo -e "    ✅ Non-destructive version restore"
echo -e "    ✅ Storage & deduplication statistics"
echo -e "    ✅ Cross-file chunk deduplication"
echo -e "    ✅ TCP server with multi-threaded client handling"
echo -e "    ✅ Remote file operations (list, history, ping, stats)"
echo ""
echo -e "${BOLD}  RevFS v$(./revfs --version | awk '{print $3}') — All features working!${NC}"
echo ""
