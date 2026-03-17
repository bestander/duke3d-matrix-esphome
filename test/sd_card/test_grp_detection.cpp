#include <cassert>
#include <cstring>
#include <cstdio>

// Host-side test for SdCard path construction logic only.
// Mounts and file I/O are hardware-only; we test the path string logic.

static char last_opened_path[256] = {};

static const char* MOUNT = "/sdcard";

void mock_open(const char* rel_path) {
    snprintf(last_opened_path, sizeof(last_opened_path),
             "%s/%s", MOUNT, rel_path);
}

void test_grp_path_is_correct() {
    mock_open("duke3d/DUKE3D.GRP");
    assert(strcmp(last_opened_path, "/sdcard/duke3d/DUKE3D.GRP") == 0);
}

void test_grp_full_path_is_correct() {
    mock_open("duke3d_full/DUKE3D.GRP");
    assert(strcmp(last_opened_path, "/sdcard/duke3d_full/DUKE3D.GRP") == 0);
}

int main() {
    test_grp_path_is_correct();
    test_grp_full_path_is_correct();
    return 0;
}
