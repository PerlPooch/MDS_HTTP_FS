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

## Filesystem API

Register the API at an application-selected route before starting the server.
The route can coexist with a browser uploader using the same filesystem root.

    MDS_HTTP_FS::addCrud(http, "/api/files", LittleFS, "/www");

| Request | Result |
| --- | --- |
| GET /api/files | JSON array of immediate children of the configured root |
| GET /api/files?dir=assets | JSON array of immediate children of that directory |
| GET /api/files?path=assets/style.css | File bytes as application/octet-stream |
| POST /api/files?path=assets/style.css | Multipart field named file. Returns 201 for a new file or 200 for a replacement |
| DELETE /api/files?path=assets/style.css | Removes the file and returns 204 |

Each list entry has a name and type. File entries also include size in bytes.
Pass URL-encoded, root-relative paths in query parameters. POST uses the query
path to select the destination and creates missing parent directories. A failed
or interrupted upload leaves the existing file in place. DELETE operates on
files. Requests for missing files return 404. Duplicate or invalid paths return
400. A file or directory type mismatch returns 409.

The optional fifth argument is an application access check for all API methods.
An omitted check leaves the route open. A false result returns 403 and prevents
filesystem access, including staging a POST. A future browser UI can serve its
own page and use this API for listing, downloads, uploads, and deletions.
