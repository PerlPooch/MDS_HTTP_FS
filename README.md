# MDS_HTTP_FS

A filesystem upload backend for `MDS_HTTP` on ESP32 and ESP8266. It takes a mounted
Arduino `fs::FS` and uses the filesystem's same-volume rename operation to replace
files. LittleFS supports this operation.

## Arduino examples

- [BrowserUpload](examples/BrowserUpload/BrowserUpload.ino) prints a URL for the
  upload form. Select a file and optionally enter a relative destination path.
- [PostOnlyUpload](examples/PostOnlyUpload/PostOnlyUpload.ino) prints a `curl`
  command. Create a local `hello.txt` and run the command to upload it.

Both sketches write under `/uploads` on LittleFS. Set `WifiSsid` and
`WifiPassword`, select a board with a filesystem partition, and open the Serial
Monitor at 115200 baud. For a blank partition, set `FormatOnMountFailure` to
`true` for the first run. Formatting erases the filesystem if mounting fails.

## Usage

Register a unique route before starting the listener. This requires the version
of `MDS_HTTP` with `MDS_HTTP::BrowserForm`.

```cpp
#include <LittleFS.h>
#include <MDS_HTTP_FS.h>

// Register the upload endpoint and its optional browser form.
void registerWebsiteUpload(MDS_HTTP& http) {
	MDS_HTTP_FS::addUpload(http, "/upload-file", LittleFS, "/www",
		MDS_HTTP::BrowserForm::Enabled);
}
```

Open `/upload-file` to select a file. The browser form can set a relative
destination path for nested files. Omit the last installer argument for a
POST-only route.

To upload from a client, send one multipart field named `file`. The multipart filename is relative to the
upload root. For example, this writes `/www/assets/style.css`, creating missing
parent directories.

```sh
curl --fail-with-body -F 'file=@style.css;filename=assets/style.css' http://BOARD_IP/upload-file
```

The backend accepts empty files. It rejects absolute filenames, empty path
components, dot segments, backslashes, and control characters. The root reserves
`.mds-http-upload.tmp` for staging. Use one uploader per root and keep the filesystem
mounted for the duration of each request. If a transfer is interrupted, the previous
file stays in place. A staging file left by a reset is reused on the next upload.
